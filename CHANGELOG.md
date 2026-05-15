# Changelog

## v0.3.0 — 2026-05-15

Headline additions are the Conference feature (Phase 1: Confrn-built
multimix bridge with phone-side UI promotion, ConfList read + action
softkeys, multi-call merge via Join / Select / Unselect, RmLastConf),
DND / CFwdALL / HuntGroup CLIs + dialplan functions wired to BLF
lamps, the Record softkey, and a service-control PRT report CLI.
Internals: helper bodies now live in `res_pjsip_cisco_endpoint.so` so
every other module shares one canonical implementation, and the
NAT'd-contact RURI rewrite moved out of `res_pjsip_cisco_unsolicited_blf`
into a global `on_tx_request` hook covering every outbound request.
Bench-tested on Asterisk 20.19, 22.9, and 23.3.

### New features

- **Conference softkey** (`res_pjsip_cisco_conference`). Phase 1
  end-to-end: the in-call Conference softkey builds a real multimix
  bridge and joins the held leg, with the post-Confrn wire dance —
  `holdretrievereq` + in-dialog Cisco-flavoured NOTIFY carrying an
  RFC 3515 sipfrag body + Event:refer NOTIFY — so the phone-side UI
  promotes to a Conference glyph and un-holds without the user
  touching anything. The Join / Select / Unselect softkeys merge
  multiple held calls into one bridge. RmLastConf removes the
  most-recently-joined participant (tracked per channel in
  `cisco_session`). The ConfList softkey returns the participant
  menu; its Mute / Remove / Update action softkeys are now wired
  through to the bridge. `cisco_keep_conference` controls whether
  the bridge survives initiator hangup.
- **DND / CFwdALL / HuntGroup CLIs and dialplan functions**
  (`res_pjsip_cisco_feature_events` + shared helpers). New CLIs
  `pjsip cisco {dnd,huntgroup,cfwdall} <endpoint> [on|off|number]`
  and dialplan functions `CISCO_DND()`, `CISCO_HUNTGROUP()`,
  `CISCO_CFWDALL()` (read + write) drive the same astdb state the
  softkey SUBSCRIBEs maintain; a write also pushes a bulkupdate
  REFER so the phone-side toggle/icon updates in real time. DND
  state additionally drives BLF lamps via a Cisco presence-state
  provider — hints can watch a DND'd line and the lamp goes red.
- **Record softkey** (`StartRecording` / `StopRecording`) in
  `res_pjsip_cisco_remotecc`. Resolves the softkey REFER's
  `<dialogid>` to the phone's channel and runs `MixMonitor` (or
  `StopMixMonitor`) on it off the SIP rx thread, defaulting to
  filename `cisco-<endpoint>-<uniqueid>.wav` under the configured
  MixMonitor directory. Override per call by setting
  `CISCO_RECORD_FILENAME` on the channel from dialplan. chan_sip's
  `cisco-usecallmanager` patch approaches the same softkey by
  creating a second SIP dialog and dispatching to extension
  `record`; in chan_pjsip a direct MixMonitor on the bridged
  channel gives the same result.
- **`pjsip cisco prt-report <endpoint> [contact]` CLI**
  (`res_pjsip_cisco_service_control`). Triggers the phone's
  Problem Report Tool upload via the same service-control NOTIFY
  channel as `check-sync` / `restart` / `reset`.

### Internals

- **Shared helpers compiled into `res_pjsip_cisco_endpoint.so`**.
  `cisco_endpoint.h` was split into six topical headers
  (`cisco_endpoint.h`, `cisco_rdata.h`, `cisco_register.h`,
  `cisco_refer.h`, `cisco_session.h`, `cisco_orig_host.h`) carrying
  declarations only; bodies live in sibling `.c` files that compile
  into the endpoint `.so` and are exported globally so every other
  module resolves them via the loader's `RTLD_GLOBAL` pass on
  `AST_MODFLAG_GLOBAL_SYMBOLS`. Same pattern stock Asterisk uses
  for `ast_sip_*` in `res_pjsip.so`. Before this, helpers were
  inline in headers and each consumer compiled its own copy.
- **NAT'd-contact RURI/To rewrite moved to a global `on_tx_request`
  hook** (`cisco_orig_host` inside `res_pjsip_cisco_endpoint.so`).
  When `res_pjsip_nat`'s `rewrite_contact=yes` leaves an
  `x-ast-orig-host` URI parameter on the Contact-derived RURI, the
  hook rewrites the Request-URI and To-URI back to the phone's
  self-advertised host:port. Cisco firmware on NAT'd phones rejected
  unsolicited NOTIFYs / REFERs with `400 Bad Request` because the
  public NAT mapping in the RURI didn't match what the phone knew
  about itself. The hook is a strict no-op for any URI without the
  parameter — LAN-registered contacts, upstream trunks, non-NAT'd
  targets all pass through unchanged. The earlier per-module
  rewrite that lived in `res_pjsip_cisco_unsolicited_blf` (and
  never covered REFER) is gone.
- **`res_pjsip_cisco_feature_events` PATH A removed**. The
  `as-feature-event` SUBSCRIBE handler turned out to be dead in
  chan_pjsip — firmware uses RemoteCC REFERs instead. The only
  remaining path is PATH B (astdb-backed feature state).
- **`Makefile` propagates `SELF_SYM` into helper objects** so the
  helper `.c` files compiled into `res_pjsip_cisco_endpoint.so` link
  against the parent module's self-symbol instead of an undefined
  one.

### Docs

- README rewritten as a user-facing feature tour with a
  compatibility table covering Asterisk 20.x → 23.x and bench-tested
  status per release.
- ARCHITECTURE.md tracks the landed Conference + RemoteCC work, the
  shared-helpers `.so` pattern, and `cisco_orig_host`'s role.

## v0.1.0 — 2026-05-13

First tagged release — an early 0.x cut: works on the maintainer's
bench but wants real-world testing across more phone models and Asterisk
versions before it earns a 1.0. Ten shared modules adding Cisco
Enterprise SIP firmware support to chan_pjsip (built and CI'd against
Asterisk 20.x / 22.x / 23.x; bench-tested on 22.9.x):

- `res_pjsip_cisco_endpoint` — `cisco` sorcery type + per-endpoint
  gating (`line_index`, `subscribe`, `subscribe_context`, `aliases`,
  `parkext`).
- `res_pjsip_cisco_pidf_body_generator` — Cisco-flavoured PIDF +
  RPID + Cisco-private rpid namespace. Supplements stock `pidf+xml`
  output and wins the `cpim-pidf+xml` generator slot before stock
  xpidf.
- `res_pjsip_cisco_register_optionsind` — REGISTER 200 OK gets
  `application/x-cisco-remotecc-response+xml` optionsind body.
- `res_pjsip_cisco_bulkupdate` — post-REGISTER unsolicited REFER with
  multipart `application/x-cisco-remotecc-request+xml` carrying
  `<dndupdate>` + `<hlogupdate>` + `<bulkupdate><contact line="N">…`.
- `res_pjsip_cisco_unsolicited_blf` — post-REGISTER unsolicited
  Event: presence NOTIFY per watched extension; the trigger that
  flips Cisco firmware's line buttons into BLF Speed Dial mode.
- `res_pjsip_cisco_service_control` — CLI: `pjsip cisco
  {check-sync,restart,reset} <endpoint> [contact]` (optional
  `[contact]` — a URI substring or `@hash` — targets one phone of a
  shared line; both args tab-complete). The restart/reset body carries
  the phone's REGISTER `Call-ID`, sourced per contact.
- `res_pjsip_cisco_feature_events` — handles Cisco DND/CFwdALL
  softkey SUBSCRIBEs and writes state to astdb.
- `res_pjsip_cisco_call_extras` — adds Cisco call signaling extras:
  `Call-Info` RemoteCC metadata, `Supported: X-cisco-sis-10.0.0`,
  callback number in Remote-Party-ID, and H.264 SDP hints.
- `res_pjsip_cisco_remotecc` — claims Cisco RemoteCC REFERs and
  implements HLog, MCID, and the Park / ParkMonitor softkeys (parks
  the call by blind-transferring the peer to `res_parking`'s parkext —
  default `700`, overridable via `parkext=` on the `type=cisco`
  object — off the SIP rx thread on its own serializer, then pushes
  the slot back to the phone as a status toast / orbit-BLF NOTIFY).
  Requires `res_parking` configured (`res_parking.conf`).
- `res_pjsip_cisco_conference` — Phase 1 conference control: the
  ConfList softkey returns a read-only `<CiscoIPPhoneMenu>` listing
  the bridge participants for the active call leg.

Plus a dialplan sample
(`conf-samples/extensions.conf.cisco-features.sample`) wiring up
`res_parking` for the Park softkey + the dialplan-only hardware-button
replacements: group pickup (`*8`), ad-hoc ConfBridge meet-me, and a
CallManager-style chained-transfer conference (`*0<ext>`). The sample
includes a print-and-stick reference card for end users.

Verified against a Cisco 7975 running Enterprise SIP firmware 9.4.2
and Asterisk 22.9.0.

### Known issues

- Automatic pushes for DND/CF state changes made outside the phone
  softkeys are not implemented. Use `pjsip cisco bulkupdate
  <endpoint>` after external astdb writes, or let the next REGISTER
  refresh phone-side state.

### Fixes during pre-release

- **Service-control `RegisterCallId` is now per-contact**. The
  restart/reset NOTIFY body must carry the Call-ID of the *target
  phone's* REGISTER transaction (the firmware 400s a mismatch). It was
  captured into astdb keyed by endpoint id, so on a shared line
  (`max_contacts > 1`) whichever phone registered last owned the slot
  and every other phone got someone else's Call-ID and ignored the
  restart. Now read straight from `contact->call_id` (the registrar
  records it per contact); the REGISTER-capture supplement and the
  `RegisterCallId` astdb family are gone.

- **`LOCAL_DOMAIN` hardcode removed**. Cisco-generated URI fragments now
  reuse the From URI host/port that Asterisk/PJSIP built on the
  outgoing `pjsip_tx_data`; Cisco-specific code mutates only the user
  part where firmware requires it. `cisco_endpoint_local_domain()`
  still has endpoint-transport and `localhost` fallbacks for paths that
  do not have a tx_data yet.

- **PIDF coexistence rework**. The `noload =>
  res_pjsip_pidf_body_generator.so` /
  `res_pjsip_xpidf_body_generator.so` directives are no longer
  required:
  - For `application/pidf+xml` we now register a body **supplement**
    (`ast_sip_pubsub_register_body_supplement`) that decorates stock
    pidf's `pjpidf_pres` tree with `<dm:person>/<e:activities>` and
    Cisco rpid extensions, instead of replacing the generator. Stock
    pidf and our module run side-by-side.
  - For `application/cpim-pidf+xml` we still register a generator
    (Cisco firmware Accepts only this subtype, and stock xpidf would
    emit the older RFC 2779 format Cisco doesn't render as BLF), but
    with `load_pri = AST_MODPRI_CHANNEL_DEPEND - 5` so we register
    before stock xpidf. Stock xpidf logs one WARNING at startup
    ("already registered") and declines. Operator config
    requirement: zero.

- **Makefile auto-derives `ASTERISK_INCLUDE_DIR` from `PJPROJECT_DIR`**.
  Previously, building with `make PJPROJECT_DIR=/path/to/asterisk-src`
  used `/usr/include/asterisk/*.h` for asterisk headers - which is
  `asterisk-dev` package content if installed, or stale leftovers from
  an older `make install` if not. On boxes that self-build asterisk
  this routinely diverges from the running binary's struct layouts and
  causes runtime SEGV when our modules read `struct ast_sip_endpoint`
  fields past the first few. Now `PJPROJECT_DIR=...` implicitly sets
  `ASTERISK_INCLUDE_DIR=$PJPROJECT_DIR/include` (when that file
  exists), keeping struct layouts in lockstep with the running binary.
