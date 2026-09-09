# Turns a binary file into a C++ source defining a byte array.
#   cmake -DINPUT=file -DOUTPUT=out.cpp -DSYMBOL=name -P embed.cmake
file(READ "${INPUT}" HEX_DATA HEX)
string(LENGTH "${HEX_DATA}" HEX_LEN)
math(EXPR BYTE_COUNT "${HEX_LEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ARRAY "${HEX_DATA}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT}; do not edit.\n#include <cstddef>\nextern const unsigned char ${SYMBOL}[] = {${ARRAY}};\nextern const size_t ${SYMBOL}_size = ${BYTE_COUNT};\n")
