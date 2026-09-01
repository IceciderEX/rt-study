#!/usr/bin/env python3
import os
import struct

def main():
    src_trace = "/home/wam/grad/s14-range-delete-study/traces/ls24_density/ls24_density_phases.bin"
    out_dir = "/home/wam/grad/s14-range-delete-study/traces/ls24_density_clean"
    os.makedirs(out_dir, exist_ok=True)
    out_trace = os.path.join(out_dir, "ls24_density_clean_phases.bin")

    if not os.path.exists(src_trace):
        print(f"Error: {src_trace} not found")
        return

    print("Generating LS24-DensityPreserved-Clean trace (replacing DeleteRange op_type=3 with No-op op_type=4)...")
    count_ops = 0
    count_noop = 0

    with open(src_trace, "rb") as fin, open(out_trace, "wb") as fout:
        while True:
            b_op = fin.read(1)
            if not b_op:
                break
            b_k1 = fin.read(8)
            b_k2 = fin.read(8)
            op = struct.unpack("<B", b_op)[0]
            k1 = struct.unpack("<Q", b_k1)[0]
            k2 = struct.unpack("<Q", b_k2)[0]
            count_ops += 1

            if op == 3: # DeleteRange
                # Replace with No-op (op_type 4)
                fout.write(struct.pack("<BQQ", 4, k1, k2))
                count_noop += 1
            else:
                fout.write(struct.pack("<BQQ", op, k1, k2))

    print(f"Generated {out_trace}: {count_ops} ops total, {count_noop} DeleteRanges converted to No-op.")

if __name__ == "__main__":
    main()
