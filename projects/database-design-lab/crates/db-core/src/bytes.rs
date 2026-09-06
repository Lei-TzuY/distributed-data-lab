use std::fmt;

use serde::{Deserialize, Deserializer, Serialize, Serializer};
use thiserror::Error;

/// A binary string represented as lowercase hexadecimal when serialized to JSON.
#[derive(Clone, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ByteString(Vec<u8>);

impl ByteString {
    /// Creates a binary string from owned bytes.
    #[must_use]
    pub fn new(bytes: Vec<u8>) -> Self {
        Self(bytes)
    }

    /// Returns the contained bytes.
    #[must_use]
    pub fn as_slice(&self) -> &[u8] {
        &self.0
    }

    /// Consumes the wrapper and returns the contained bytes.
    #[must_use]
    pub fn into_vec(self) -> Vec<u8> {
        self.0
    }

    /// Encodes the bytes as lowercase hexadecimal without a prefix.
    #[must_use]
    pub fn to_hex(&self) -> String {
        const HEX: &[u8; 16] = b"0123456789abcdef";
        let mut encoded = String::with_capacity(self.0.len().saturating_mul(2));
        for byte in &self.0 {
            encoded.push(char::from(HEX[usize::from(byte >> 4)]));
            encoded.push(char::from(HEX[usize::from(byte & 0x0f)]));
        }
        encoded
    }

    /// Decodes an even-length hexadecimal string.
    pub fn from_hex(encoded: &str) -> Result<Self, ByteStringError> {
        if encoded.len() % 2 != 0 {
            return Err(ByteStringError::OddLength(encoded.len()));
        }

        let mut bytes = Vec::with_capacity(encoded.len() / 2);
        for (index, pair) in encoded.as_bytes().chunks_exact(2).enumerate() {
            let high = decode_nibble(pair[0]).ok_or(ByteStringError::InvalidDigit {
                index: index * 2,
                byte: pair[0],
            })?;
            let low = decode_nibble(pair[1]).ok_or(ByteStringError::InvalidDigit {
                index: index * 2 + 1,
                byte: pair[1],
            })?;
            bytes.push((high << 4) | low);
        }
        Ok(Self(bytes))
    }
}

fn decode_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

impl From<Vec<u8>> for ByteString {
    fn from(value: Vec<u8>) -> Self {
        Self::new(value)
    }
}

impl From<&[u8]> for ByteString {
    fn from(value: &[u8]) -> Self {
        Self::new(value.to_vec())
    }
}

impl AsRef<[u8]> for ByteString {
    fn as_ref(&self) -> &[u8] {
        self.as_slice()
    }
}

impl fmt::Debug for ByteString {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "0x{}", self.to_hex())
    }
}

impl fmt::Display for ByteString {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.to_hex())
    }
}

impl Serialize for ByteString {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_str(&self.to_hex())
    }
}

impl<'de> Deserialize<'de> for ByteString {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let encoded = String::deserialize(deserializer)?;
        Self::from_hex(&encoded).map_err(serde::de::Error::custom)
    }
}

/// Error returned when a JSON hexadecimal byte string is invalid.
#[derive(Debug, Error, PartialEq, Eq)]
pub enum ByteStringError {
    /// The input contains an odd number of hexadecimal digits.
    #[error("hexadecimal byte string has odd length {0}")]
    OddLength(usize),
    /// The input contains a non-hexadecimal byte.
    #[error("invalid hexadecimal digit 0x{byte:02x} at index {index}")]
    InvalidDigit {
        /// Byte offset in the encoded string.
        index: usize,
        /// Invalid byte.
        byte: u8,
    },
}

#[cfg(test)]
mod tests {
    use super::ByteString;

    #[test]
    fn serde_round_trip_preserves_binary_data() {
        let bytes = ByteString::from(vec![0x00, 0x7f, 0x80, 0xff]);
        let json = serde_json::to_string(&bytes).expect("serialize byte string");
        assert_eq!(json, "\"007f80ff\"");
        let decoded: ByteString = serde_json::from_str(&json).expect("deserialize byte string");
        assert_eq!(decoded, bytes);
    }

    #[test]
    fn empty_string_represents_empty_bytes() {
        assert!(ByteString::from_hex("")
            .expect("decode empty bytes")
            .as_slice()
            .is_empty());
    }
}
