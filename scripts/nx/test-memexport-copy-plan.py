"""Check copy-model ranges, byte-order identity and unsupported-state guards."""
from pathlib import Path
import importlib.util
import struct
import unittest

spec = importlib.util.spec_from_file_location('copy_model', Path(__file__).with_name('plan-memexport-copies.py'))
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)
LINEAR = 'd4991999678dd7e26c2c876e06b078cc87b875f5addf173bfdb1beb64eb00bf5'
STRIDED = '0c9ced036fbfff79ae9c6c9d83eb256c58aa57e98d7c504e1699f0430182e490'


def fixture(count=2, elements=16):
    regs = [0] * 0x5003
    regs[0x2100] = 0xffffff
    regs[0x21fc] = (count << 16) | 0x81
    regs[0x2180] = 7 << 24
    regs[0x4000:0x4004] = [(0x18000000 >> 2) | 0x40000000, 0x4b000000, 0x4b021a01, 0x4b000000 | elements]
    regs[0x4000 + 254 * 4:0x4004 + 254 * 4] = [0x42000000, 0x41400000, 0, 0]
    regs[0x4000 + 255 * 4:0x4004 + 255 * 4] = [0, 0x3f800000, 0x40800000, 0x41000000]
    regs[0x4800 + 95 * 2:0x4802 + 95 * 2] = [0x1a000003, (1024 // 4 << 2) | 2]
    record = {'primitive': 1, 'indexed': False, 'explicit_major': False, 'count': count}
    return record, regs


class CopyModelTests(unittest.TestCase):
    def test_linear_ranges(self):
        record, regs = fixture()
        result = model.plan(record, regs, LINEAR)
        self.assertEqual(result['ranges'], [{'source': 0x1a000000, 'target': 0x18000000, 'bytes': 128}])
        self.assertEqual(result['clipped_elements'], 0)

    def test_strided_gaps_and_clipping(self):
        record, regs = fixture(count=4, elements=64)
        result = model.plan(record, regs, STRIDED)
        self.assertEqual(result['ranges'], [
            {'source': 0x1a000000, 'target': 0x18000000, 'bytes': 128},
            {'source': 0x1a000100, 'target': 0x18000100, 'bytes': 128}])
        self.assertEqual((result['written_bytes'], result['clipped_elements'], result['untouched_destination_bytes']),
                         (256, 32, 256))

    def test_partial_final_element_block(self):
        record, regs = fixture(elements=10)
        result = model.plan(record, regs, LINEAR)
        self.assertEqual(result['written_bytes'], 80)
        self.assertEqual(result['clipped_elements'], 6)

    def test_reserved_format_bits_are_ignored(self):
        record, regs = fixture()
        original = model.plan(record, regs, LINEAR)
        regs[0x4002] |= 0xc0f8
        self.assertEqual(model.plan(record, regs, LINEAR), original)

    def test_unsupported_state_and_overlap(self):
        mutations = [
            (0x2180, 0), (0x2102, 1), (0x2101, 1), (0x2100, 0), (0x2307, 1),
            (0x4002, 0x4b021a02), (0x4002, 0x4b001a01), (0x4002, 0x4b0a1a01),
            (0x4001, 0x4b000001), (0x4800 + 95 * 2, 0x18000003),
            (0x4801 + 95 * 2, (16 // 4 << 2) | 2),
            (0x4801 + 95 * 2, (1024 // 4 << 2) | 1), (0x21fc, (2 << 16) | 1),
        ]
        for index, value in mutations:
            with self.subTest(register=hex(index), value=hex(value)):
                record, regs = fixture()
                regs[index] = value
                with self.assertRaises(ValueError):
                    model.plan(record, regs, LINEAR)
        record, regs = fixture()
        with self.assertRaises(ValueError):
            model.plan(record, regs, 'unrecognized')
        record['indexed'] = True
        with self.assertRaises(ValueError):
            model.plan(record, regs, LINEAR)

    def test_all_uint16_values_survive_reference_conversion(self):
        for value in range(65536):
            data = struct.pack('<4H', value, 65535 - value, value ^ 0xaaaa, value ^ 0x5555)
            if model.byte_order_reference(data) != data:
                self.fail(f'Fetch/swizzle/export conversion changed uint16 {value}')
        # Also exercise each individual bit position in the raw 8-byte element.
        for bit in range(64):
            data = (1 << bit).to_bytes(8, 'little')
            self.assertEqual(model.byte_order_reference(data), data)


if __name__ == '__main__':
    unittest.main()
