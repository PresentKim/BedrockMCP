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

func encode(pk packet.Packet) []byte {
	var buf bytes.Buffer
	w := protocol.NewWriter(&buf, 0)
	header := pk.ID()
	var head []byte
	head = binary.AppendUvarint(head, uint64(header))
	pk.Marshal(w)
	return append(head, buf.Bytes()...)
}

func writeLog(t *testing.T, records []record) string {
	path := filepath.Join(t.TempDir(), "test.bmpl")
	var out bytes.Buffer
	out.WriteString(magic)
	_ = binary.Write(&out, binary.LittleEndian, uint32(version))
	for _, r := range records {
		fixed := make([]byte, 24)
		binary.LittleEndian.PutUint64(fixed[0:], uint64(r.Time.UnixMicro()))
		fixed[8] = r.Dir
		fixed[9] = r.Side
		binary.LittleEndian.PutUint64(fixed[12:], r.Conn)
		binary.LittleEndian.PutUint32(fixed[20:], uint32(len(r.Data)))
		out.Write(fixed)
		out.Write(r.Data)
	}
	if err := os.WriteFile(path, out.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestDecodesServerTextPacket(t *testing.T) {
	path := writeLog(t, []record{{Dir: dirReceived, Side: sideClient, Data: encode(&packet.Text{TextType: packet.TextTypeRaw, Message: "hello"})}})
	d := newDecoder()
	var got decoded
	if err := readLog(path, func(r record) bool { got = d.decode(r); return true }); err != nil {
		t.Fatal(err)
	}
	text, ok := got.Packet.(*packet.Text)
	if got.Err != nil || !ok || text.Message != "hello" || got.Remaining != 0 {
		t.Fatalf("got %+v err=%v", got.Packet, got.Err)
	}
}

func TestServerSideSentUsesServerPool(t *testing.T) {
	if !(record{Dir: dirSent, Side: sideServer}).fromServer() || (record{Dir: dirSent, Side: sideClient}).fromServer() {
		t.Fatal("direction mapping is wrong")
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
	recs := []record{
		{Dir: dirReceived, Side: sideClient, Data: encode(&packet.ResourcePackDataInfo{UUID: id, DataChunkSize: uint32(half), ChunkCount: 2, Size: uint64(len(data))})},
		{Dir: dirReceived, Side: sideClient, Data: encode(&packet.ResourcePackChunkData{UUID: id, ChunkIndex: 1, DataOffset: uint64(half), Data: data[half:]})},
		{Dir: dirReceived, Side: sideClient, Data: encode(&packet.ResourcePackChunkData{UUID: id, ChunkIndex: 0, Data: data[:half]})},
	}
	out := t.TempDir()
	if err := runPacks(writeLog(t, recs), out); err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(filepath.Join(out, id, "manifest.json"))
	if err != nil || !strings.Contains(string(body), "format_version") {
		t.Fatalf("manifest not extracted: %v %q", err, body)
	}
}
