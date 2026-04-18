# Test TLS Certificates

Self-signed certificate and key for **local HTTPS testing only**. Do not use
in production. Private key is committed on purpose so running the example
HTTPS config does not require any extra setup.

## Files

- `server.crt` — self-signed X.509 certificate (10-year validity)
- `server.key` — unencrypted RSA 2048 private key

## Properties

- Subject: `CN=localhost`
- SAN: `DNS:localhost, IP:127.0.0.1, IP:0.0.0.0`

## Usage

From the repo root:

```bash
cd data && ./path/to/ag-server config.https.json
```

Clients must be built with `-DAG_SSL_VERIFY=OFF` (or have `server.crt` added
to the system trust store) to accept this self-signed certificate.

## Regenerate

```bash
cd data/certs
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
    -days 3650 -nodes \
    -subj "/C=CN/ST=Test/L=Test/O=AGUpdater/OU=Dev/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:0.0.0.0"
```
