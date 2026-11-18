package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"time"
)

const (
	magic   = "BMPL"
	version = 1

	dirSent     = 0
	dirReceived = 1

	sideUnknown = 0
	sideClient  = 1
	sideServer  = 2
)

type record struct {
	Seq  int
	Time time.Time
	Dir  byte
	Side byte
	Conn uint64
	Data []byte
}

func (r record) fromServer() bool {
	if r.Side == sideServer {
		return r.Dir == dirSent
	}
	return r.Dir == dirReceived
}

func (r record) dirName() string {
	if r.Dir == dirSent {
		return "sent"
	}
	return "received"
}

func (r record) sideName() string {
	switch r.Side {
	case sideClient:
		return "client"
	case sideServer:
		return "server"
	}
	return "unknown"
}

func readLog(path string, each func(record) bool) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	in := bufio.NewReaderSize(f, 1<<20)
	head := make([]byte, 8)
	if _, err := io.ReadFull(in, head); err != nil {
		return fmt.Errorf("read header: %w", err)
	}
	if string(head[:4]) != magic || binary.LittleEndian.Uint32(head[4:]) != version {
		return fmt.Errorf("%s is not a version %d packet log", path, version)
	}
	fixed := make([]byte, 24)
	for seq := 1; ; seq++ {
		if _, err := io.ReadFull(in, fixed); err != nil {
			if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
				return nil
			}
			return err
		}
		size := binary.LittleEndian.Uint32(fixed[20:])
		data := make([]byte, size)
		if _, err := io.ReadFull(in, data); err != nil {
			return nil
		}
		rec := record{
			Seq:  seq,
			Time: time.UnixMicro(int64(binary.LittleEndian.Uint64(fixed[0:]))),
			Dir:  fixed[8],
			Side: fixed[9],
			Conn: binary.LittleEndian.Uint64(fixed[12:]),
			Data: data,
		}
		if !each(rec) {
			return nil
		}
	}
}
