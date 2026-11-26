# Intel initializer capture

Current checkpoint: **host3 / services 0.18.0 / private transport 0.10.0**.
Use the [stage/parent ownership test instructions](SGX545-STAGE-OWNER.md).
The new capture requires host3; the working host2 remains usable with the older
CPU diagnostics, but cannot provide the new parent lease.

The [transport 0.9.0 boot-input capture](../research/sgx545/d2500hn-boot-inputs.json)
has passed. Its 49 ordered writes and original allocation relationships are
recorded and audited; another input-discovery run is not needed.
Transport 0.10.0 uses the same initializer payload to test the actual stage/parent
lease while its native binding is retained. GPU execution remains disabled.

The [firmware boot design](SGX545-FIRMWARE-BOOT.md) explains the controller,
register limitations, resource retention and remaining live MMIO integration.
