package main

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"math"
)

func decodeMsgpack(r *bytes.Reader) (any, error) {
	tag, err := r.ReadByte()
	if err != nil {
		return nil, err
	}
	switch {
	case tag <= 0x7f:
		return int64(tag), nil
	case tag >= 0xe0:
		return int64(int8(tag)), nil
	case tag&0xf0 == 0x80:
		return readMap(r, int(tag&0x0f))
	case tag&0xf0 == 0x90:
		return readArray(r, int(tag&0x0f))
	case tag&0xe0 == 0xa0:
		return readString(r, int(tag&0x1f))
	}
	switch tag {
	case 0xc0:
		return nil, nil
	case 0xc2:
		return false, nil
	case 0xc3:
		return true, nil
	case 0xc4, 0xc5, 0xc6:
		n, err := readLength(r, tag-0xc4)
		if err != nil {
			return nil, err
		}
		b, err := readBytes(r, n)
		return hex.EncodeToString(b), err
	case 0xca:
		b, err := readBytes(r, 4)
		if err != nil {
			return nil, err
		}
		return float64(math.Float32frombits(binary.BigEndian.Uint32(b))), nil
	case 0xcb:
		b, err := readBytes(r, 8)
		if err != nil {
			return nil, err
		}
		return math.Float64frombits(binary.BigEndian.Uint64(b)), nil
	case 0xcc, 0xcd, 0xce, 0xcf:
		size := 1 << (tag - 0xcc)
		b, err := readBytes(r, size)
		if err != nil {
			return nil, err
		}
		var v uint64
		for _, c := range b {
			v = v<<8 | uint64(c)
		}
		return v, nil
	case 0xd0, 0xd1, 0xd2, 0xd3:
		size := 1 << (tag - 0xd0)
		b, err := readBytes(r, size)
		if err != nil {
			return nil, err
		}
		var v uint64
		for _, c := range b {
			v = v<<8 | uint64(c)
		}
		shift := 64 - 8*size
		return int64(v<<shift) >> shift, nil
	case 0xd9, 0xda, 0xdb:
		n, err := readLength(r, tag-0xd9)
		if err != nil {
			return nil, err
		}
		return readString(r, n)
	case 0xdc, 0xdd:
		n, err := readLength(r, tag-0xdc+1)
		if err != nil {
			return nil, err
		}
		return readArray(r, n)
	case 0xde, 0xdf:
		n, err := readLength(r, tag-0xde+1)
		if err != nil {
			return nil, err
		}
		return readMap(r, n)
	}
	return nil, fmt.Errorf("unsupported msgpack tag 0x%02x", tag)
}

func readLength(r *bytes.Reader, sizeCode byte) (int, error) {
	b, err := readBytes(r, 1<<sizeCode)
	if err != nil {
		return 0, err
	}
	n := 0
	for _, c := range b {
		n = n<<8 | int(c)
	}
	return n, nil
}

func readBytes(r *bytes.Reader, n int) ([]byte, error) {
	if n < 0 || n > r.Len() {
		return nil, io.ErrUnexpectedEOF
	}
	b := make([]byte, n)
	_, err := io.ReadFull(r, b)
	return b, err
}

func readString(r *bytes.Reader, n int) (string, error) {
	b, err := readBytes(r, n)
	return string(b), err
}

func readArray(r *bytes.Reader, n int) ([]any, error) {
	out := make([]any, 0, n)
	for i := 0; i < n; i++ {
		v, err := decodeMsgpack(r)
		if err != nil {
			return out, err
		}
		out = append(out, v)
	}
	return out, nil
}

func readMap(r *bytes.Reader, n int) (map[string]any, error) {
	out := make(map[string]any, n)
	for i := 0; i < n; i++ {
		k, err := decodeMsgpack(r)
		if err != nil {
			return out, err
		}
		v, err := decodeMsgpack(r)
		if err != nil {
			return out, err
		}
		out[fmt.Sprint(k)] = v
	}
	return out, nil
}
