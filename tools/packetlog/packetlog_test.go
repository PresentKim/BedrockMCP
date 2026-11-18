package main

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/sandertv/gophertunnel/minecraft/protocol"
	"github.com/sandertv/gophertunnel/minecraft/protocol/packet"
)

type fakePacket struct {
	pk       packet.Packet
	outbound bool
	failed   bool
	errDoc   []byte
}

func body(pk packet.Packet) []byte {
	var buf bytes.Buffer
	pk.Marshal(protocol.NewWriter(&buf, 0))
	return buf.Bytes()
}

func writeCapture(t *testing.T, packets []fakePacket) *capture {
	dir := t.TempDir()
	var cap, index bytes.Buffer
	cap.WriteString(capMagic)
	_ = binary.Write(&cap, binary.LittleEndian, uint32(capVersion))
	_ = binary.Write(&cap, binary.LittleEndian, uint32(4))
	cap.WriteString("test")
	for i, fp := range packets {
		b := body(fp.pk)
		blob := append([]byte(nil), b...)
		flags := uint8(0)
		if fp.outbound {
			flags |= flagOutbond
		}
		if !fp.failed {
			flags |= flagDecoded
		}
		if fp.errDoc != nil {
			flags |= flagError
			blob = binary.LittleEndian.AppendUint32(blob, uint32(len(fp.errDoc)))
			blob = append(blob, fp.errDoc...)
		}
		_ = binary.Write(&cap, binary.LittleEndian, uint32(len(blob)))
		offset := uint64(cap.Len())
		cap.Write(blob)
		_ = binary.Write(&cap, binary.LittleEndian, uint32(len(blob)))
		e := make([]byte, entrySize)
		binary.LittleEndian.PutUint64(e[0:], offset)
		binary.LittleEndian.PutUint64(e[8:], uint64(i*1000))
		binary.LittleEndian.PutUint32(e[16:], uint32(len(b)))
		binary.LittleEndian.PutUint32(e[20:], uint32(len(blob)))
		binary.LittleEndian.PutUint16(e[28:], uint16(fp.pk.ID()))
		e[31] = flags
		index.Write(e)
	}
	path := filepath.Join(dir, "spyglass_test.cap")
	if err := os.WriteFile(path, cap.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "spyglass_test.index"), index.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	c, err := findCapture(path)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func msgpackReason(reason string) []byte {
	out := []byte{0x81, 0xa6}
	out = append(out, "reason"...)
	out = append(out, 0xd9, byte(len(reason)))
	return append(out, reason...)
}

func allEntries(t *testing.T, c *capture) []entry {
	var out []entry
	if err := c.entries(1, 0, func(e entry) bool { out = append(out, e); return true }); err != nil {
		t.Fatal(err)
	}
	return out
}

func TestReadsEntriesAndDecodesFields(t *testing.T) {
	c := writeCapture(t, []fakePacket{
		{pk: &packet.Text{TextType: packet.TextTypeRaw, Message: "hello"}},
		{pk: &packet.RequestNetworkSettings{ClientProtocol: 2225}, outbound: true},
	})
	entries := allEntries(t, c)
	if len(entries) != 2 || entries[1].Number != 2 || !entries[1].outbound() {
		t.Fatalf("entries %+v", entries)
	}
	b, err := c.read(entries[0])
	if err != nil {
		t.Fatal(err)
	}
	pk := newDecoder().decode(entries[0].ID, b.Body, false)
	if text, ok := pk.Packet.(*packet.Text); !ok || pk.Err != nil || text.Message != "hello" {
		t.Fatalf("decoded %+v err=%v", pk.Packet, pk.Err)
	}
}

func TestReadsClientErrorReason(t *testing.T) {
	c := writeCapture(t, []fakePacket{{pk: &packet.SetTime{Time: 5}, failed: true, errDoc: msgpackReason("bad string")}})
	e := allEntries(t, c)[0]
	b, err := c.read(e)
	if err != nil || !e.failed() || reasonOf(b.Error) != `"bad string"` {
		t.Fatalf("failed=%v error=%v err=%v", e.failed(), b.Error, err)
	}
}

func TestFilterMatchesNameDirectionAndId(t *testing.T) {
	f, err := parseFilter("text", "0x09", "in", false, false, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	if !f.match(entry{ID: 9}, "Text") || f.match(entry{ID: 9, Flags: flagOutbond}, "Text") || f.match(entry{ID: 10}, "Text") {
		t.Fatal("filter mismatch")
	}
}

func TestReassemblesResourcePack(t *testing.T) {
	var archive bytes.Buffer
	zw := zip.NewWriter(&archive)
	f, _ := zw.Create("manifest.json")
	_, _ = f.Write([]byte(`{"format_version":2}`))
	_ = zw.Close()
	data := archive.Bytes()
	half := len(data) / 2
	id := "11111111-2222-3333-4444-555555555555_1.0.0"
	c := writeCapture(t, []fakePacket{
		{pk: &packet.ResourcePackDataInfo{UUID: id, DataChunkSize: uint32(half), ChunkCount: 2, Size: uint64(len(data))}},
		{pk: &packet.ResourcePackChunkData{UUID: id, ChunkIndex: 1, DataOffset: uint64(half), Data: data[half:]}},
		{pk: &packet.ResourcePackChunkData{UUID: id, ChunkIndex: 0, Data: data[:half]}},
	})
	out := t.TempDir()
	if err := runPacks(c, 0, 0, out); err != nil {
		t.Fatal(err)
	}
	manifest, err := os.ReadFile(filepath.Join(out, id, "manifest.json"))
	if err != nil || !strings.Contains(string(manifest), "format_version") {
		t.Fatalf("manifest not extracted: %v %q", err, manifest)
	}
}
