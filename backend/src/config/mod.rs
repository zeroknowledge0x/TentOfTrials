use anyhow::Result;
use std::fmt;
use serde::{Deserialize, Serialize};
use std::path::Path;

/// Environment variable names for runtime configuration.
const ENV_HOST: &str = "TOT_BACKEND_HOST";
const ENV_PORT: &str = "TOT_BACKEND_PORT";
const ENV_LOG_LEVEL: &str = "TOT_LOG_LEVEL";
const ENV_ENABLE_EXPERIMENTAL: &str = "TOT_ENABLE_EXPERIMENTAL";

/// Errors that can occur when building a [`Config`] from environment variables.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigError {
    /// The port value is not a valid number or exceeds 65535.
    InvalidPort(String),
    /// The boolean value is not one of "true", "false", "1", "0".
    InvalidBoolean(String),
}

impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ConfigError::InvalidPort(msg) => write!(f, "invalid port: {msg}"),
            ConfigError::InvalidBoolean(msg) => write!(f, "invalid boolean: {msg}"),
        }
    }
}

impl std::error::Error for ConfigError {}

/// Runtime configuration populated from environment variables.
///
/// Use [`Config::from_env`] to build an instance. Each field falls back to a
/// safe default when the corresponding environment variable is unset.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    /// Address the server binds to (default `0.0.0.0`).
    pub host: String,
    /// Port the server listens on (default `8080`).
    pub port: u16,
    /// Log level passed to the tracing subscriber (default `info`).
    pub log_level: String,
    /// Whether to enable experimental features (default `false`).
    pub enable_experimental: bool,
}

impl Config {
    /// Build a [`Config`] by reading environment variables.
    ///
    /// | Variable | Default | Notes |
    /// |---|---|---|
    /// | `TOT_BACKEND_HOST` | `0.0.0.0` | — |
    /// | `TOT_BACKEND_PORT` | `8080` | Must be a valid port number (0–65535) |
    /// | `TOT_LOG_LEVEL` | `info` | — |
    /// | `TOT_ENABLE_EXPERIMENTAL` | `false` | Accepts `true`, `false`, `1`, `0` |
    pub fn from_env() -> Result<Self, ConfigError> {
        let host = std::env::var(ENV_HOST).unwrap_or_else(|_| "0.0.0.0".to_string());

        let port = match std::env::var(ENV_PORT) {
            Ok(val) => parse_port(&val)?,
            Err(_) => 8080,
        };

        let log_level = std::env::var(ENV_LOG_LEVEL).unwrap_or_else(|_| "info".to_string());

        let enable_experimental = match std::env::var(ENV_ENABLE_EXPERIMENTAL) {
            Ok(val) => parse_bool_env(&val)?,
            Err(_) => false,
        };

        Ok(Config {
            host,
            port,
            log_level,
            enable_experimental,
        })
    }
}

/// Parse a port string, validating it is numeric and within range.
fn parse_port(raw: &str) -> Result<u16, ConfigError> {
    let port: u16 = raw
        .parse()
        .map_err(|_| ConfigError::InvalidPort(format!("'{raw}' is not a valid port number (expected 0–65535)")))?;
    Ok(port)
}

/// Parse a boolean environment variable, accepting only `true`/`false`/`1`/`0`.
fn parse_bool_env(raw: &str) -> Result<bool, ConfigError> {
    match raw {
        "true" | "1" => Ok(true),
        "false" | "0" => Ok(false),
        other => Err(ConfigError::InvalidBoolean(format!(
            "'{other}' is not a valid boolean (expected one of: true, false, 1, 0)"
        ))),
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServiceConfig {
    pub name: String,
    pub version: String,
    pub host: String,
    pub port: u16,
    pub tls_enabled: bool,
    pub tls_cert_path: Option<String>,
    pub tls_key_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RegistryConfig {
    pub backend: String,
    pub endpoints: Vec<String>,
    pub heartbeat_interval_ms: u64,
    pub ttl_seconds: u64,
    pub replication_factor: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiscoveryConfig {
    pub provider: String,
    pub namespace: String,
    pub tags: Vec<String>,
    pub health_check_path: String,
    pub health_check_interval_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MessagingConfig {
    pub broker_type: String,
    pub uris: Vec<String>,
    pub consumer_group: String,
    pub max_retries: u32,
    pub retry_backoff_ms: u64,
    pub batch_size: u32,
    pub compression: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RootConfig {
    pub service: ServiceConfig,
    pub registry: RegistryConfig,
    pub discovery: DiscoveryConfig,
    pub messaging: MessagingConfig,
}

impl Default for RootConfig {
    fn default() -> Self {
        Self {
            service: ServiceConfig {
                name: "tent-backend".into(),
                version: "0.1.0".into(),
                host: "0.0.0.0".into(),
                port: 8080,
                tls_enabled: false,
                tls_cert_path: None,
                tls_key_path: None,
            },
            registry: RegistryConfig {
                backend: "etcd".into(),
                endpoints: vec!["localhost:2379".into()],
                heartbeat_interval_ms: 5000,
                ttl_seconds: 30,
                replication_factor: 3,
            },
            discovery: DiscoveryConfig {
                provider: "consul".into(),
                namespace: "tent".into(),
                tags: vec!["microservice".into(), "orchestration".into()],
                health_check_path: "/health".into(),
                health_check_interval_ms: 10000,
            },
            messaging: MessagingConfig {
                broker_type: "kafka".into(),
                uris: vec!["localhost:9092".into()],
                consumer_group: "tent-consumers".into(),
                max_retries: 3,
                retry_backoff_ms: 1000,
                batch_size: 500,
                compression: "snappy".into(),
            },
        }
    }
}

pub async fn load_config(path: &str) -> Result<RootConfig> {
    let path = Path::new(path);
    if path.exists() {
        let contents = tokio::fs::read_to_string(path).await?;
        let config: RootConfig = toml::from_str(&contents)?;
        tracing::info!("configuration loaded from {}", path.display());
        Ok(config)
    } else {
        tracing::warn!(
            "config file {} not found, using defaults",
            path.display()
        );
        Ok(RootConfig::default())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Clear the four env vars so tests start from a clean slate.
    fn clear_env() {
        for key in [ENV_HOST, ENV_PORT, ENV_LOG_LEVEL, ENV_ENABLE_EXPERIMENTAL] {
            std::env::remove_var(key);
        }
    }

    #[test]
    fn defaults_when_no_env_vars_set() {
        clear_env();
        let cfg = Config::from_env().expect("from_env should succeed with defaults");
        assert_eq!(cfg.host, "0.0.0.0");
        assert_eq!(cfg.port, 8080);
        assert_eq!(cfg.log_level, "info");
        assert!(!cfg.enable_experimental);
    }

    #[test]
    fn valid_overrides() {
        clear_env();
        std::env::set_var(ENV_HOST, "127.0.0.1");
        std::env::set_var(ENV_PORT, "3000");
        std::env::set_var(ENV_LOG_LEVEL, "debug");
        std::env::set_var(ENV_ENABLE_EXPERIMENTAL, "true");

        let cfg = Config::from_env().expect("from_env should succeed with valid overrides");
        assert_eq!(cfg.host, "127.0.0.1");
        assert_eq!(cfg.port, 3000);
        assert_eq!(cfg.log_level, "debug");
        assert!(cfg.enable_experimental);

        clear_env();
    }

    #[test]
    fn invalid_port_returns_error() {
        clear_env();
        std::env::set_var(ENV_PORT, "not_a_number");
        let err = Config::from_env().unwrap_err();
        assert!(matches!(err, ConfigError::InvalidPort(_)));
        assert!(err.to_string().contains("not_a_number"));

        std::env::set_var(ENV_PORT, "99999");
        let err = Config::from_env().unwrap_err();
        assert!(matches!(err, ConfigError::InvalidPort(_)));
        assert!(err.to_string().contains("99999"));

        clear_env();
    }

    #[test]
    fn invalid_boolean_returns_error() {
        clear_env();
        std::env::set_var(ENV_ENABLE_EXPERIMENTAL, "yes");
        let err = Config::from_env().unwrap_err();
        assert!(matches!(err, ConfigError::InvalidBoolean(_)));
        assert!(err.to_string().contains("yes"));

        std::env::set_var(ENV_ENABLE_EXPERIMENTAL, "TRUE");
        let err = Config::from_env().unwrap_err();
        assert!(matches!(err, ConfigError::InvalidBoolean(_)));

        clear_env();
    }

    #[test]
    fn boolean_zero_and_one() {
        clear_env();
        std::env::set_var(ENV_ENABLE_EXPERIMENTAL, "1");
        assert!(Config::from_env().unwrap().enable_experimental);

        std::env::set_var(ENV_ENABLE_EXPERIMENTAL, "0");
        assert!(!Config::from_env().unwrap().enable_experimental);

        clear_env();
    }

    #[test]
    fn boundary_port_values() {
        clear_env();
        std::env::set_var(ENV_PORT, "0");
        assert_eq!(Config::from_env().unwrap().port, 0);

        std::env::set_var(ENV_PORT, "65535");
        assert_eq!(Config::from_env().unwrap().port, 65535);

        clear_env();
    }
}
