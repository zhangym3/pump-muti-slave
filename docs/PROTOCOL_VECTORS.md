# Protocol contract vectors

The master and C3 slave keep independent codec implementations. Both host-test
targets run the same vector in `test/core_tests.cpp` so a codec change cannot
silently alter the wire contract.

## Request vector

`REQUEST`, peer `0x1234`, encoded with little-endian fields:

```text
01 44 33 22 11 08 07 06 05 04 03 02 01
CC BB AA 99 01 00 00 80 F4 01 00 00 60 EA 00 00
88 77 66 55 DD CC BB AA
```

The vector is 37 bytes. It represents `Action::Start`, epoch `0x11223344`,
client `0x0102030405060708`, sequence `0x99AABBCC`, mask `0x80000001`,
500 mL at 60,000 ms, and stop key `0x55667788:0xAABBCCDD`.

The tests also assert Company ID `0x02E5`, Model ID `0x0001`, group address
`0xC001`, and the opcode range `0xC0..0xCB`.
