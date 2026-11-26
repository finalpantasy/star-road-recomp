"""Keep direct calls and registration on the public symbol; rename only the body."""
import re
import sys
from pathlib import Path


def wrap(chunk, source, registry):
    for row in registry.splitlines():
        owner, address = re.fullmatch(r"LEAF\((main_[0-9]+), ([0-9a-f]+), \w+\)", row).groups()
        if owner != chunk:
            continue
        symbol = "f_" + address
        declaration = f"void {symbol}(Ctx *restrict c)"
        definition = declaration + " {"
        if source.count(definition) != 1:
            raise ValueError(f"expected one definition of {symbol} in this lifted chunk")
        source = source.replace(definition, declaration + ";\n" + definition.replace(symbol, symbol + "_lifted"))
    return source


if __name__ == "__main__":
    chunk, manifest, destination = map(Path, sys.argv[1:])
    destination.write_text(wrap(chunk.stem, chunk.read_text(), manifest.read_text()))
