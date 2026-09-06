use serde_json::Value;

use crate::signaling::{base91_decode, looks_like_room_code};

const WEB_CODE_PREFIX: &str = "FBS2:";

#[must_use]
pub fn looks_like_web_compat_code(text: &str) -> bool {
    extract_web_room(text).is_some()
}

#[must_use]
pub fn extract_web_room(text: &str) -> Option<String> {
    let clean: String = text
        .chars()
        .filter(|character| !character.is_whitespace())
        .collect();
    if looks_like_room_code(&clean) {
        return Some(clean);
    }

    let encoded = clean.strip_prefix(WEB_CODE_PREFIX)?;
    let raw = base91_decode(encoded).ok()?;
    let value: Value = serde_json::from_slice(&raw).ok()?;
    let code = value.as_object()?.get("code")?.as_str()?;
    looks_like_room_code(code).then(|| code.to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::signaling::base91_encode;

    #[test]
    fn native_room_codes_are_web_compatible() {
        assert_eq!(extract_web_room("ABCD12"), Some("ABCD12".to_owned()));
        assert_eq!(extract_web_room("  AB CD 12\n"), Some("ABCD12".to_owned()));
    }

    #[test]
    fn fbs2_envelope_matches_cpp_contract() {
        let json = br#"{"code":"ABCD12"}"#;
        let text = format!("FBS2:{}", base91_encode(json));
        assert_eq!(extract_web_room(&text), Some("ABCD12".to_owned()));
        assert!(looks_like_web_compat_code(&text));
    }

    #[test]
    fn malformed_fbs2_codes_fail_closed() {
        assert_eq!(extract_web_room("FBS2:not valid base91"), None);
        let wrong_shape = format!("FBS2:{}", base91_encode(br#"{"room":"ABCD12"}"#));
        assert_eq!(extract_web_room(&wrong_shape), None);
    }
}
