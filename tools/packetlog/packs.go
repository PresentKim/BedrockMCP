package main

import (
	"archive/zip"
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/sandertv/gophertunnel/minecraft/protocol/packet"
)

type download struct {
	id     string
	info   *packet.ResourcePackDataInfo
	chunks map[uint32][]byte
}

func runPacks(path, outDir string) error {
	d := newDecoder()
	keys := map[string]string{}
	downloads := map[string]*download{}
	get := func(id string) *download {
		if dl, ok := downloads[id]; ok {
			return dl
		}
		dl := &download{id: id, chunks: map[uint32][]byte{}}
		downloads[id] = dl
		return dl
	}
	err := readLog(path, func(rec record) bool {
		pk := d.decode(rec)
		if pk.Err != nil {
			return true
		}
		switch p := pk.Packet.(type) {
		case *packet.ResourcePacksInfo:
			for _, tp := range p.TexturePacks {
				if tp.ContentKey != "" {
					keys[tp.UUID.String()] = tp.ContentKey
				}
			}
		case *packet.ResourcePackDataInfo:
			get(p.UUID).info = p
		case *packet.ResourcePackChunkData:
			get(p.UUID).chunks[p.ChunkIndex] = append([]byte(nil), p.Data...)
		}
		return true
	})
	if err != nil {
		return err
	}
	if len(downloads) == 0 {
		fmt.Println("no ResourcePackChunkData in the log")
		return nil
	}
	ids := make([]string, 0, len(downloads))
	for id := range downloads {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		fmt.Println(savePack(downloads[id], keys[strings.SplitN(id, "_", 2)[0]], outDir))
	}
	return nil
}

func savePack(dl *download, key, outDir string) string {
	name := sanitize(dl.id)
	indexes := make([]uint32, 0, len(dl.chunks))
	for i := range dl.chunks {
		indexes = append(indexes, i)
	}
	sort.Slice(indexes, func(a, b int) bool { return indexes[a] < indexes[b] })
	var data bytes.Buffer
	for _, i := range indexes {
		data.Write(dl.chunks[i])
	}
	status := fmt.Sprintf("%s: %d chunks, %d bytes", dl.id, len(dl.chunks), data.Len())
	if dl.info != nil {
		status += fmt.Sprintf(" (expected %d chunks, %d bytes)", dl.info.ChunkCount, dl.info.Size)
		if uint32(len(dl.chunks)) != dl.info.ChunkCount || uint64(data.Len()) != dl.info.Size {
			status += " INCOMPLETE"
		}
	}
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return status + "; " + err.Error()
	}
	archive := filepath.Join(outDir, name+".zip")
	if err := os.WriteFile(archive, data.Bytes(), 0o644); err != nil {
		return status + "; " + err.Error()
	}
	status += "; saved " + archive
	files, err := extract(data.Bytes(), filepath.Join(outDir, name), key)
	if err != nil {
		return status + "; extract: " + err.Error()
	}
	status += fmt.Sprintf("; extracted %d files to %s", files, filepath.Join(outDir, name))
	if key != "" {
		status += " (decrypted with the content key from ResourcePacksInfo)"
	}
	return status
}

func extract(data []byte, dir, key string) (int, error) {
	reader, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return 0, err
	}
	contents := map[string][]byte{}
	for _, f := range reader.File {
		if f.FileInfo().IsDir() {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			return 0, err
		}
		body, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			return 0, err
		}
		contents[f.Name] = body
	}
	if key != "" {
		if err := decryptPack(contents, key); err != nil {
			return 0, err
		}
	}
	count := 0
	for name, body := range contents {
		target := filepath.Join(dir, filepath.FromSlash(name))
		if !strings.HasPrefix(filepath.Clean(target), filepath.Clean(dir)+string(os.PathSeparator)) {
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return count, err
		}
		if err := os.WriteFile(target, body, 0o644); err != nil {
			return count, err
		}
		count++
	}
	return count, nil
}

// Assumed Marketplace pack encryption, not yet checked against a real encrypted pack:
// contents.json has a 256-byte header followed by an AES-256-CFB8 body keyed by the
// pack's content key, each listed file uses its own key, and the IV is the key's first 16 bytes.
func decryptPack(contents map[string][]byte, key string) error {
	var root string
	var index []byte
	for name, body := range contents {
		if strings.HasSuffix(name, "contents.json") && strings.Count(strings.TrimSuffix(name, "contents.json"), "/") <= 1 {
			root = strings.TrimSuffix(name, "contents.json")
			index = body
			break
		}
	}
	if len(index) <= 0x100 {
		return fmt.Errorf("content key given but no encrypted contents.json found")
	}
	plain, err := cfb8Decrypt([]byte(key), index[0x100:])
	if err != nil {
		return err
	}
	var listing struct {
		Content []struct {
			Path string `json:"path"`
			Key  string `json:"key"`
		} `json:"content"`
	}
	if err := json.Unmarshal(plain, &listing); err != nil {
		return fmt.Errorf("contents.json did not decrypt to JSON: %w", err)
	}
	contents[root+"contents.json"] = plain
	for _, entry := range listing.Content {
		if entry.Key == "" {
			continue
		}
		body, ok := contents[root+entry.Path]
		if !ok {
			continue
		}
		out, err := cfb8Decrypt([]byte(entry.Key), body)
		if err != nil {
			return fmt.Errorf("%s: %w", entry.Path, err)
		}
		contents[root+entry.Path] = out
	}
	return nil
}

func cfb8Decrypt(key, data []byte) ([]byte, error) {
	if len(key) != 32 {
		return nil, fmt.Errorf("key is %d bytes, want 32", len(key))
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	return cfb8(block, key[:16], data), nil
}

func cfb8(block cipher.Block, iv, data []byte) []byte {
	shift := append([]byte(nil), iv...)
	stream := make([]byte, block.BlockSize())
	out := make([]byte, len(data))
	for i, c := range data {
		block.Encrypt(stream, shift)
		out[i] = c ^ stream[0]
		copy(shift, shift[1:])
		shift[len(shift)-1] = c
	}
	return out
}

func sanitize(s string) string {
	return strings.Map(func(r rune) rune {
		if strings.ContainsRune(`<>:"/\|?*`, r) || r < 32 {
			return '_'
		}
		return r
	}, s)
}
