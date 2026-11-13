// Package fdt reads the few things the flasher needs from a flattened device tree (.dtb): the root node's "model" and
// "compatible". Our own reader (the format is the devicetree specification's, v0.4 §5), no dependency.
package fdt

import (
	"bytes"
	"encoding/binary"
	"errors"
	"strings"
)

const magic = 0xd00dfeed

// Info is what a device tree says it is for.
type Info struct {
	Model      string
	Compatible []string
}

// Parse reads the root node's model and compatible properties.
func Parse(b []byte) (Info, error) {
	var in Info
	if len(b) < 40 || binary.BigEndian.Uint32(b) != magic {
		return in, errors.New("not a device tree")
	}
	total := int(binary.BigEndian.Uint32(b[4:]))
	offStruct := int(binary.BigEndian.Uint32(b[8:]))
	offStrings := int(binary.BigEndian.Uint32(b[12:]))
	if total > len(b) || offStruct >= total || offStrings >= total {
		return in, errors.New("truncated device tree")
	}
	str := func(off int) string {
		p := offStrings + off
		if p < 0 || p >= total {
			return ""
		}
		e := bytes.IndexByte(b[p:total], 0)
		if e < 0 {
			return ""
		}
		return string(b[p : p+e])
	}
	p, depth := offStruct, 0
	for p+4 <= total {
		tok := binary.BigEndian.Uint32(b[p:])
		p += 4
		switch tok {
		case 1: // BEGIN_NODE
			e := bytes.IndexByte(b[p:total], 0)
			if e < 0 {
				return in, errors.New("bad node name")
			}
			p += (e + 1 + 3) &^ 3
			depth++
		case 2: // END_NODE
			depth--
			if depth <= 0 {
				return in, nil
			}
		case 3: // PROP
			if p+8 > total {
				return in, errors.New("bad property")
			}
			ln := int(binary.BigEndian.Uint32(b[p:]))
			name := str(int(binary.BigEndian.Uint32(b[p+4:])))
			p += 8
			if p+ln > total {
				return in, errors.New("bad property length")
			}
			if depth == 1 {
				v := b[p : p+ln]
				switch name {
				case "model":
					in.Model = strings.TrimRight(string(v), "\x00")
				case "compatible":
					for _, s := range strings.Split(strings.TrimRight(string(v), "\x00"), "\x00") {
						if s != "" {
							in.Compatible = append(in.Compatible, s)
						}
					}
				}
			}
			p += (ln + 3) &^ 3
		case 4: // NOP
		case 9: // END
			return in, nil
		default:
			return in, errors.New("bad token")
		}
	}
	return in, nil
}
