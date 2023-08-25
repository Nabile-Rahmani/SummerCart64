use super::error::Error;
use serial2::SerialPort;
use std::{
    collections::VecDeque,
    io::{BufReader, BufWriter, ErrorKind, Read, Write},
    net::TcpStream,
    thread,
    time::{Duration, Instant},
};

pub enum DataType {
    Command,
    Response,
    Packet,
    KeepAlive,
}

impl From<DataType> for u32 {
    fn from(value: DataType) -> Self {
        match value {
            DataType::Command => 1,
            DataType::Response => 2,
            DataType::Packet => 3,
            DataType::KeepAlive => 0xCAFEBEEF,
        }
    }
}

impl TryFrom<u32> for DataType {
    type Error = Error;
    fn try_from(value: u32) -> Result<Self, Self::Error> {
        Ok(match value {
            1 => Self::Command,
            2 => Self::Response,
            3 => Self::Packet,
            0xCAFEBEEF => Self::KeepAlive,
            _ => return Err(Error::new("Unknown data type")),
        })
    }
}

pub struct Command {
    pub id: u8,
    pub args: [u32; 2],
    pub data: Vec<u8>,
}

pub struct Response {
    pub id: u8,
    pub data: Vec<u8>,
    pub error: bool,
}

pub struct Packet {
    pub id: u8,
    pub data: Vec<u8>,
}

pub struct Serial {
    serial: SerialPort,
}

impl Serial {
    fn reset(&self) -> Result<(), Error> {
        const WAIT_DURATION: Duration = Duration::from_millis(10);
        const RETRY_COUNT: i32 = 100;

        self.serial.set_dtr(true)?;
        for n in 0..=RETRY_COUNT {
            self.serial.discard_buffers()?;
            thread::sleep(WAIT_DURATION);
            if self.serial.read_dsr()? {
                break;
            }
            if n == RETRY_COUNT {
                return Err(Error::new("Couldn't reset SC64 device (on)"));
            }
        }

        self.serial.set_dtr(false)?;
        for n in 0..=RETRY_COUNT {
            thread::sleep(WAIT_DURATION);
            if !self.serial.read_dsr()? {
                break;
            }
            if n == RETRY_COUNT {
                return Err(Error::new("Couldn't reset SC64 device (off)"));
            }
        }

        Ok(())
    }

    fn read_data(&self, buffer: &mut [u8], block: bool) -> Result<Option<()>, Error> {
        let timeout = Instant::now();
        let mut position = 0;
        let length = buffer.len();
        while position < length {
            if timeout.elapsed() > Duration::from_secs(10) {
                return Err(Error::new("Serial read timeout"));
            }
            match self.serial.read(&mut buffer[position..length]) {
                Ok(0) => return Err(Error::new("Unexpected end of serial data")),
                Ok(bytes) => position += bytes,
                Err(error) => match error.kind() {
                    ErrorKind::Interrupted | ErrorKind::TimedOut | ErrorKind::WouldBlock => {
                        if !block && position == 0 {
                            return Ok(None);
                        }
                    }
                    _ => return Err(error.into()),
                },
            }
        }
        Ok(Some(()))
    }

    fn read_exact(&self, buffer: &mut [u8]) -> Result<(), Error> {
        match self.read_data(buffer, true)? {
            Some(()) => Ok(()),
            None => Err(Error::new("Unexpected end of serial data")),
        }
    }

    fn read_header(&self, block: bool) -> Result<Option<[u8; 4]>, Error> {
        let mut header = [0u8; 4];
        Ok(self.read_data(&mut header, block)?.map(|_| header))
    }

    pub fn send_command(&self, command: &Command) -> Result<(), Error> {
        self.serial.write_all(b"CMD")?;
        self.serial.write_all(&command.id.to_be_bytes())?;
        self.serial.write_all(&command.args[0].to_be_bytes())?;
        self.serial.write_all(&command.args[1].to_be_bytes())?;

        self.serial.write_all(&command.data)?;

        self.serial.flush()?;

        Ok(())
    }

    pub fn process_incoming_data(
        &self,
        data_type: DataType,
        packets: &mut VecDeque<Packet>,
    ) -> Result<Option<Response>, Error> {
        let block = matches!(data_type, DataType::Response);
        while let Some(header) = self.read_header(block)? {
            let (packet_token, error) = (match &header[0..3] {
                b"CMP" => Ok((false, false)),
                b"PKT" => Ok((true, false)),
                b"ERR" => Ok((false, true)),
                _ => Err(Error::new("Unknown response token")),
            })?;
            let id = header[3];

            let mut buffer = [0u8; 4];

            self.read_exact(&mut buffer)?;
            let length = u32::from_be_bytes(buffer) as usize;

            let mut data = vec![0u8; length];
            self.read_exact(&mut data)?;

            if packet_token {
                packets.push_back(Packet { id, data });
                if matches!(data_type, DataType::Packet) {
                    break;
                }
            } else {
                return Ok(Some(Response { id, error, data }));
            }
        }

        Ok(None)
    }
}

pub fn new_serial(port: &str) -> Result<Serial, Error> {
    let mut serial = SerialPort::open(port, 115_200)?;
    serial.set_write_timeout(Duration::from_secs(10))?;
    serial.set_read_timeout(Duration::from_millis(10))?;
    let backend = Serial { serial };
    backend.reset()?;
    Ok(backend)
}

trait Backend {
    fn send_command(&mut self, command: &Command) -> Result<(), Error>;
    fn process_incoming_data(
        &mut self,
        data_type: DataType,
        packets: &mut VecDeque<Packet>,
    ) -> Result<Option<Response>, Error>;
}

struct SerialBackend {
    inner: Serial,
}

impl Backend for SerialBackend {
    fn send_command(&mut self, command: &Command) -> Result<(), Error> {
        self.inner.send_command(command)
    }

    fn process_incoming_data(
        &mut self,
        data_type: DataType,
        packets: &mut VecDeque<Packet>,
    ) -> Result<Option<Response>, Error> {
        self.inner.process_incoming_data(data_type, packets)
    }
}

fn new_serial_backend(port: &str) -> Result<SerialBackend, Error> {
    let backend = SerialBackend {
        inner: new_serial(port)?,
    };
    Ok(backend)
}

struct TcpBackend {
    reader: BufReader<TcpStream>,
    writer: BufWriter<TcpStream>,
}

impl TcpBackend {
    fn read_data(&mut self, buffer: &mut [u8], block: bool) -> Result<Option<()>, Error> {
        let timeout = Instant::now();
        let mut position = 0;
        let length = buffer.len();
        while position < length {
            if timeout.elapsed() > Duration::from_secs(10) {
                return Err(Error::new("Stream read timeout"));
            }
            match self.reader.read(&mut buffer[position..length]) {
                Ok(0) => return Err(Error::new("Unexpected end of stream data")),
                Ok(bytes) => position += bytes,
                Err(error) => match error.kind() {
                    ErrorKind::Interrupted | ErrorKind::TimedOut | ErrorKind::WouldBlock => {
                        if !block && position == 0 {
                            return Ok(None);
                        }
                    }
                    _ => return Err(error.into()),
                },
            }
        }
        Ok(Some(()))
    }

    fn read_exact(&mut self, buffer: &mut [u8]) -> Result<(), Error> {
        match self.read_data(buffer, true)? {
            Some(()) => Ok(()),
            None => Err(Error::new("Unexpected end of stream data")),
        }
    }

    fn read_header(&mut self, block: bool) -> Result<Option<[u8; 4]>, Error> {
        let mut header = [0u8; 4];
        Ok(self.read_data(&mut header, block)?.map(|_| header))
    }
}

impl Backend for TcpBackend {
    fn send_command(&mut self, command: &Command) -> Result<(), Error> {
        let payload_data_type: u32 = DataType::Command.into();
        self.writer.write_all(&payload_data_type.to_be_bytes())?;

        self.writer.write_all(&command.id.to_be_bytes())?;
        self.writer.write_all(&command.args[0].to_be_bytes())?;
        self.writer.write_all(&command.args[1].to_be_bytes())?;

        let command_data_length = command.data.len() as u32;
        self.writer.write_all(&command_data_length.to_be_bytes())?;
        self.writer.write_all(&command.data)?;

        self.writer.flush()?;

        Ok(())
    }

    fn process_incoming_data(
        &mut self,
        data_type: DataType,
        packets: &mut VecDeque<Packet>,
    ) -> Result<Option<Response>, Error> {
        let block = matches!(data_type, DataType::Response);
        while let Some(header) = self.read_header(block)? {
            let payload_data_type: DataType = u32::from_be_bytes(header).try_into()?;
            let mut buffer = [0u8; 4];
            match payload_data_type {
                DataType::Response => {
                    let mut response_info = vec![0u8; 2];
                    self.read_exact(&mut response_info)?;

                    self.read_exact(&mut buffer)?;
                    let response_data_length = u32::from_be_bytes(buffer) as usize;

                    let mut data = vec![0u8; response_data_length];
                    self.read_exact(&mut data)?;

                    return Ok(Some(Response {
                        id: response_info[0],
                        error: response_info[1] != 0,
                        data,
                    }));
                }
                DataType::Packet => {
                    let mut packet_info = vec![0u8; 1];
                    self.read_exact(&mut packet_info)?;

                    self.read_exact(&mut buffer)?;
                    let packet_data_length = u32::from_be_bytes(buffer) as usize;

                    let mut data = vec![0u8; packet_data_length];
                    self.read_exact(&mut data)?;

                    packets.push_back(Packet {
                        id: packet_info[0],
                        data,
                    });
                    if matches!(data_type, DataType::Packet) {
                        break;
                    }
                }
                DataType::KeepAlive => {}
                _ => return Err(Error::new("Unexpected payload data type received")),
            };
        }

        Ok(None)
    }
}

fn new_tcp_backend(address: &str) -> Result<TcpBackend, Error> {
    let stream = match TcpStream::connect(address) {
        Ok(stream) => {
            stream.set_write_timeout(Some(Duration::from_secs(10)))?;
            stream.set_read_timeout(Some(Duration::from_millis(10)))?;
            stream
        }
        Err(error) => {
            return Err(Error::new(
                format!("Couldn't connect to [{address}]: {error}").as_str(),
            ))
        }
    };
    let reader = BufReader::new(stream.try_clone()?);
    let writer = BufWriter::new(stream.try_clone()?);
    Ok(TcpBackend { reader, writer })
}

pub struct Link {
    backend: Box<dyn Backend>,
    packets: VecDeque<Packet>,
}

impl Link {
    pub fn execute_command(&mut self, command: &Command) -> Result<Vec<u8>, Error> {
        self.execute_command_raw(command, false, false)
    }

    pub fn execute_command_raw(
        &mut self,
        command: &Command,
        no_response: bool,
        ignore_error: bool,
    ) -> Result<Vec<u8>, Error> {
        self.backend.send_command(command)?;
        if no_response {
            return Ok(vec![]);
        }
        let response = self.receive_response()?;
        if command.id != response.id {
            return Err(Error::new("Command response ID didn't match"));
        }
        if !ignore_error && response.error {
            return Err(Error::new("Command response error"));
        }
        Ok(response.data)
    }

    fn receive_response(&mut self) -> Result<Response, Error> {
        match self
            .backend
            .process_incoming_data(DataType::Response, &mut self.packets)
        {
            Ok(response) => match response {
                Some(response) => Ok(response),
                None => Err(Error::new("No response was received")),
            },
            Err(error) => Err(Error::new(
                format!("Command response error: {error}").as_str(),
            )),
        }
    }

    pub fn receive_packet(&mut self) -> Result<Option<Packet>, Error> {
        if self.packets.len() == 0 {
            let response = self
                .backend
                .process_incoming_data(DataType::Packet, &mut self.packets)?;
            if response.is_some() {
                return Err(Error::new("Unexpected command response in data stream"));
            }
        }
        Ok(self.packets.pop_front())
    }
}

pub fn new_local(port: &str) -> Result<Link, Error> {
    Ok(Link {
        backend: Box::new(new_serial_backend(port)?),
        packets: VecDeque::new(),
    })
}

pub fn new_remote(address: &str) -> Result<Link, Error> {
    Ok(Link {
        backend: Box::new(new_tcp_backend(address)?),
        packets: VecDeque::new(),
    })
}

pub struct LocalDevice {
    pub port: String,
    pub serial_number: String,
}

pub fn list_local_devices() -> Result<Vec<LocalDevice>, Error> {
    const SC64_VID: u16 = 0x0403;
    const SC64_PID: u16 = 0x6014;
    const SC64_SID: &str = "SC64";

    let mut serial_devices: Vec<LocalDevice> = Vec::new();

    for device in serialport::available_ports()?.into_iter() {
        if let serialport::SerialPortType::UsbPort(info) = device.port_type {
            let serial_number = info.serial_number.unwrap_or("".to_string());
            if info.vid == SC64_VID && info.pid == SC64_PID && serial_number.starts_with(SC64_SID) {
                serial_devices.push(LocalDevice {
                    port: device.port_name,
                    serial_number,
                });
            }
        }
    }

    if serial_devices.len() == 0 {
        return Err(Error::new("No SC64 devices found"));
    }

    return Ok(serial_devices);
}
