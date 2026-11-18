package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// Spyglass v0.4.0 capture layout (src/spyglass/overlay/store.cpp, format version 2):
// <name>.cap starts with "SPYGCAP\0", u32 version, u32 length and the client name, then
// holds one record per packet as u32 length, blob, u32 length. A blob is the packet body
// (payload without the varuint header) followed by an optional length-prefixed msgpack
// error document and an optional length-prefixed msgpack fields document.
// <name>.index holds one 32-byte entry per packet in capture order.
const (
	capMagic    = "SPYGCAP\x00"
	capVersion  = 2
	entrySize   = 32
	flagOutbond = 1 << 0
	flagDecoded = 1 << 1
	flagError   = 1 << 2
	flagFields  = 1 << 3
)

type entry struct {
	Number  uint64
	Offset  uint64
	Micros  uint64
	BodyLen uint32
	BlobLen uint32
	Unread  uint32
	ID      int16
	SubID   uint8
	Flags   uint8
}

func (e entry) outbound() bool { return e.Flags&flagOutbond != 0 }
func (e entry) failed() bool   { return e.Flags&flagDecoded == 0 }
func (e entry) hasError() bool { return e.Flags&flagError != 0 }

func (e entry) dirName() string {
	if e.outbound() {
		return "out"
	}
	return "in"
}

type capture struct {
	Path   string
	Index  string
	Client string
}

func findCapture(path string) (*capture, error) {
	if path == "" {
		matches, _ := filepath.Glob(filepath.Join(os.TempDir(), "spyglass_*.cap"))
		if len(matches) == 0 {
			return nil, fmt.Errorf("no spyglass capture in %s; spyglass is not injected, or the game closed (spyglass deletes its capture on exit)", os.TempDir())
		}
		sort.Slice(matches, func(a, b int) bool { return modTime(matches[a]).After(modTime(matches[b])) })
		path = matches[0]
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	head := make([]byte, 16)
	if _, err := io.ReadFull(f, head); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if string(head[:8]) != capMagic {
		return nil, fmt.Errorf("%s is not a spyglass capture", path)
	}
	if v := binary.LittleEndian.Uint32(head[8:]); v != capVersion {
		return nil, fmt.Errorf("%s is capture format %d; this reader knows format %d (spyglass v0.4.0)", path, v, capVersion)
	}
	name := make([]byte, binary.LittleEndian.Uint32(head[12:]))
	if _, err := io.ReadFull(f, name); err != nil {
		return nil, err
	}
	return &capture{
		Path:   path,
		Index:  strings.TrimSuffix(path, ".cap") + ".index",
		Client: string(name),
	}, nil
}

func modTime(path string) time.Time {
	info, err := os.Stat(path)
	if err != nil {
		return time.Time{}
	}
	return info.ModTime()
}

func (c *capture) count() (uint64, error) {
	f, err := os.Open(c.Index)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	end, err := f.Seek(0, io.SeekEnd)
	if err != nil {
		return 0, err
	}
	return uint64(end) / entrySize, nil
}

func (c *capture) entries(from, to uint64, each func(entry) bool) error {
	total, err := c.count()
	if err != nil {
		return err
	}
	if from < 1 {
		from = 1
	}
	if to == 0 || to > total {
		to = total
	}
	if from > to {
		return nil
	}
	f, err := os.Open(c.Index)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := f.Seek(int64(from-1)*entrySize, io.SeekStart); err != nil {
		return err
	}
	buf := make([]byte, entrySize*1024)
	number := from
	for number <= to {
		want := int(to-number+1) * entrySize
		if want > len(buf) {
			want = len(buf)
		}
		n, err := io.ReadFull(f, buf[:want])
		n -= n % entrySize
		for off := 0; off < n; off += entrySize {
			b := buf[off : off+entrySize]
			e := entry{
				Number:  number,
				Offset:  binary.LittleEndian.Uint64(b[0:]),
				Micros:  binary.LittleEndian.Uint64(b[8:]),
				BodyLen: binary.LittleEndian.Uint32(b[16:]),
				BlobLen: binary.LittleEndian.Uint32(b[20:]),
				Unread:  binary.LittleEndian.Uint32(b[24:]),
				ID:      int16(binary.LittleEndian.Uint16(b[28:])),
				SubID:   b[30],
				Flags:   b[31],
			}
			number++
			if !each(e) {
				return nil
			}
		}
		if err != nil {
			return nil
		}
	}
	return nil
}

type blob struct {
	Body   []byte
	Error  any
	Fields any
}

func (c *capture) read(e entry) (blob, error) {
	f, err := os.Open(c.Path)
	if err != nil {
		return blob{}, err
	}
	defer f.Close()
	raw := make([]byte, e.BlobLen)
	if _, err := f.ReadAt(raw, int64(e.Offset)); err != nil {
		return blob{}, err
	}
	out := blob{Body: raw[:e.BodyLen]}
	rest := raw[e.BodyLen:]
	take := func() any {
		if len(rest) < 4 {
			return nil
		}
		n := binary.LittleEndian.Uint32(rest)
		if int(n) > len(rest)-4 {
			return nil
		}
		doc, err := decodeMsgpack(bytes.NewReader(rest[4 : 4+n]))
		rest = rest[4+n:]
		if err != nil {
			return map[string]any{"msgpack_error": err.Error()}
		}
		return doc
	}
	if e.hasError() {
		out.Error = take()
	}
	if e.Flags&flagFields != 0 {
		out.Fields = take()
	}
	return out, nil
}
