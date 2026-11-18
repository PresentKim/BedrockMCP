package main

import (
	"flag"
	"fmt"
	"os"
	"strings"
	"time"
)

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	fs := flag.NewFlagSet(os.Args[1], flag.ExitOnError)
	path := fs.String("capture", "", "spyglass capture (.cap); default is the newest in %TEMP%")
	names := fs.String("name", "", "comma-separated packet name substrings")
	ids := fs.String("id", "", "comma-separated packet ids (decimal or 0x hex)")
	dir := fs.String("dir", "", "in (received by the client) or out (written by the client)")
	failed := fs.Bool("failed", false, "only packets the client failed to decode")
	unread := fs.Bool("unread", false, "only packets the client left bytes of unread")
	from := fs.Uint64("from", 0, "first packet number")
	to := fs.Uint64("to", 0, "last packet number")
	last := fs.Int("last", 0, "show only the newest N matches")
	limit := fs.Int("limit", 50, "maximum matches, 0 for all")
	fields := fs.Bool("fields", true, "decode fields with gophertunnel")
	maxJSON := fs.Int("max-json", 800, "truncate each packet's JSON, 0 for no limit")
	number := fs.Uint64("n", 0, "packet number for get")
	maxHex := fs.Int("max-hex", 2048, "bytes of the body to dump for get, 0 for all")
	after := fs.Int64("after", -1, "wait: packet number to start after, default the current newest")
	timeout := fs.Int("timeout-ms", 10000, "wait: how long to wait")
	out := fs.String("out", "", "packs: output directory")
	_ = fs.Parse(os.Args[2:])

	c, err := findCapture(*path)
	exit(err)
	p := &printer{cap: c, dec: newDecoder(), fields: *fields, maxJSON: *maxJSON}
	f, err := parseFilter(*names, *ids, *dir, *failed, *unread, *from, *to)
	exit(err)
	switch os.Args[1] {
	case "info":
		exit(runInfo(c))
	case "count":
		total, err := c.count()
		exit(err)
		fmt.Println(total)
	case "find":
		exit(runFind(c, f, *last, *limit, p))
	case "get":
		exit(runGet(c, *number, *maxHex))
	case "wait":
		exit(runWait(c, f, *after, time.Duration(*timeout)*time.Millisecond, p))
	case "failures":
		exit(runFailures(c))
	case "packs":
		if *out == "" {
			exit(fmt.Errorf("packs needs -out"))
		}
		exit(runPacks(c, *from, *to, *out))
	default:
		usage()
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: packetlog info|count|find|get|wait|failures|packs [flags]; run with -h after a command for flags")
	os.Exit(2)
}

func exit(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func splitList(s string) []string {
	var out []string
	for _, part := range strings.Split(s, ",") {
		if part = strings.TrimSpace(part); part != "" {
			out = append(out, strings.ToLower(part))
		}
	}
	return out
}

func matchesAny(name string, filters []string) bool {
	lower := strings.ToLower(name)
	for _, f := range filters {
		if strings.Contains(lower, f) {
			return true
		}
	}
	return false
}
