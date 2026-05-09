# Experiments

Scratch scripts used while fixing issue #3.

## `make_png.c`

Generates a tiny 4x4 RGBA PNG (`test.png`) used to smoke-test `png2vtf`
end-to-end after the source-level fixes.

Build and run (from the repo root):

```sh
cc -O2 -Wall -Wextra $(pkg-config --cflags libpng) \
    -o experiments/make_png experiments/make_png.c \
    $(pkg-config --libs libpng)
./experiments/make_png
../png2vtf experiments/test.png experiments/test.vtf
```

Expected first 16 bytes of `test.vtf`:

```
56 54 46 00 07 00 00 00 02 00 00 00 00 00 00 00
```

- `56 54 46 00` — signature `VTF\0`
- `07 00 00 00 02 00 00 00` — version 7.2
- `02 00 00 00 00 00 00 00` — `log2(width)=2`, `log2(height)=2`
  (confirms the `<math.h>` and `write_vtf_header` fixes are functional)
