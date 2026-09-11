# ikcp (patched)

Upstream: https://github.com/skywind3000/kcp (public domain / MIT).

Star Rail uses a 28-byte KCP header instead of the stock 24: a 4-byte session
`token` follows `conv`, both little-endian. The vendored copy is patched for it:

- `IKCP_OVERHEAD` 24 -> 28
- `token` field on `IKCPSEG` and `IKCPCB`
- `ikcp_encode_seg` / `ikcp_input` write and verify it
- `ikcp_settoken(kcp, token)` to set it after `ikcp_create`

Handshake packets carry conv/token as **big-endian** u32; the KCP header carries
them little-endian. That byte reversal is what the real client expects.
