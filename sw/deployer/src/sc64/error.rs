use std::fmt::{Display, Formatter, Result};

#[derive(Debug, Clone)]
pub struct Error {
    description: String,
}

impl Error {
    pub fn new(description: &str) -> Self {
        Error {
            description: format!("{}", description),
        }
    }
}

impl std::error::Error for Error {}

impl Display for Error {
    fn fmt(&self, f: &mut Formatter) -> Result {
        write!(f, "{}", self.description.as_str())
    }
}

impl From<std::io::Error> for Error {
    fn from(value: std::io::Error) -> Self {
        Error::new(format!("IO error: {}", value).as_str())
    }
}

impl From<serialport::Error> for Error {
    fn from(value: serialport::Error) -> Self {
        Error::new(format!("SerialPort error: {}", value.description).as_str())
    }
}
