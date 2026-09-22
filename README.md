# mod_markdown 1.1.1

Apache 2.4 Markdown handler backed by libcmark-gfm. Version 1.1 publishes an
in-memory renderer so other modules can render authorized or generated
Markdown without exposing a physical file.

## Provider API

Include `mod_markdown.h` and retrieve `markdown-renderer` / `cmark-gfm` / `1`
with `ap_lookup_provider`. Validate both `abi_version` and `struct_size` before
calling `render`.

The renderer accepts an arbitrary byte buffer and optional title, stylesheet,
raw representation URI, and full-document flag. Returned HTML belongs to the
request pool.

The handler can also call one `markdown-preprocessor/1` provider before
rendering. Configure `MarkdownPreprocessor frontmatter` to use
mod_markdown_frontmatter.

## Build

Install Apache development headers, libcmark-gfm development headers,
`pkg-config`, and a C compiler, then run:

```sh
make
sudo make install
```

If the `.pc` file is in a nonstandard location:

```sh
PKG_CONFIG_PATH=/usr/share/pkgconfig/pkgconfig make
```
