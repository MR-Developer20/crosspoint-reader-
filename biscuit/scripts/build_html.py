import os
import re
import gzip

SRC_DIR = "src"

# Keywords after which a '/' begins a regex literal (not division).
_REGEX_KW = {'return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete',
             'void', 'do', 'else', 'yield', 'await', 'case', 'throw'}


def _is_regex(prev_sig: str, prev_word: str) -> bool:
    if prev_sig == '':
        return True
    if prev_sig in "(,=:[!&|?{};+-*%^~<>":
        return True
    if (prev_sig.isalnum() or prev_sig in '_$') and prev_word in _REGEX_KW:
        return True
    return False


def strip_js_comments(s: str) -> str:
    """Remove // and /* */ comments from JS, preserving strings, template
    literals (including ${...} expressions), and regex literals byte-for-byte.
    Only deletes comment spans found in code context; any ambiguity keeps us out
    of code state and merely under-strips (never corrupts)."""
    out = []
    n = len(s)
    i = 0
    stack = [['code', 0]]      # ['code', brace_depth] or ['tmpl']
    prev_sig = ''
    word = ''
    prev_word = ''
    while i < n:
        c = s[i]
        d = s[i + 1] if i + 1 < n else ''
        if stack[-1][0] == 'code':
            if c.isalnum() or c in '_$':
                word += c
            elif word:
                prev_word = word
                word = ''
            if c == "'" or c == '"':
                q = c
                out.append(c)
                i += 1
                while i < n:
                    ch = s[i]
                    out.append(ch)
                    if ch == '\\' and i + 1 < n:
                        out.append(s[i + 1])
                        i += 2
                        continue
                    i += 1
                    if ch == q:
                        break
                prev_sig = q
                continue
            if c == '`':
                out.append(c)
                i += 1
                stack.append(['tmpl'])
                prev_sig = '`'
                continue
            if c == '/' and d == '/':
                while i < n and s[i] != '\n':
                    i += 1
                continue
            if c == '/' and d == '*':
                i += 2
                while i < n and not (s[i] == '*' and i + 1 < n and s[i + 1] == '/'):
                    i += 1
                i = min(i + 2, n)
                continue
            if c == '/' and _is_regex(prev_sig, prev_word):
                out.append(c)
                i += 1
                in_class = False
                while i < n:
                    ch = s[i]
                    out.append(ch)
                    if ch == '\\' and i + 1 < n:
                        out.append(s[i + 1])
                        i += 2
                        continue
                    i += 1
                    if ch == '[':
                        in_class = True
                    elif ch == ']':
                        in_class = False
                    elif ch == '/' and not in_class:
                        break
                    elif ch == '\n':
                        break
                prev_sig = '/'
                continue
            if c == '{':
                stack[-1][1] += 1
            elif c == '}':
                if stack[-1][1] == 0 and len(stack) > 1:
                    stack.pop()
                    out.append(c)
                    i += 1
                    continue
                elif stack[-1][1] > 0:
                    stack[-1][1] -= 1
            out.append(c)
            if not c.isspace():
                prev_sig = c
            i += 1
        else:  # inside a template literal: copy verbatim until ` or ${
            word = ''
            if c == '\\' and i + 1 < n:
                out.append(c)
                out.append(s[i + 1])
                i += 2
                continue
            if c == '`':
                out.append(c)
                i += 1
                stack.pop()
                prev_sig = '`'
                continue
            if c == '$' and d == '{':
                out.append('${')
                i += 2
                stack.append(['code', 0])
                continue
            out.append(c)
            i += 1
    return ''.join(out)


def strip_css_comments(s: str) -> str:
    """Remove /* */ comments from CSS, preserving quoted strings."""
    out = []
    n = len(s)
    i = 0
    while i < n:
        c = s[i]
        d = s[i + 1] if i + 1 < n else ''
        if c in "'\"":
            q = c
            out.append(c)
            i += 1
            while i < n:
                ch = s[i]
                out.append(ch)
                if ch == '\\' and i + 1 < n:
                    out.append(s[i + 1])
                    i += 2
                    continue
                i += 1
                if ch == q:
                    break
            continue
        if c == '/' and d == '*':
            i += 2
            while i < n and not (s[i] == '*' and i + 1 < n and s[i + 1] == '/'):
                i += 1
            i = min(i + 2, n)
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def _process_preserved_block(block: str) -> str:
    """Strip comments from the inner content of a <script>/<style> block while
    leaving <pre>/<code>/<textarea> (and the tags themselves) untouched."""
    m = re.match(r'(<(script|style)([^>]*)>)([\s\S]*)(</\2\s*>)$', block, flags=re.IGNORECASE)
    if not m:
        return block
    open_tag, tag, inner, close_tag = m.group(1), m.group(2).lower(), m.group(4), m.group(5)
    if tag == 'script':
        inner = strip_js_comments(inner)
    else:  # style
        inner = strip_css_comments(inner)
    return open_tag + inner + close_tag


def minify_html(html: str) -> str:
    # Tags where whitespace should be preserved
    preserve_tags = ['pre', 'code', 'textarea', 'script', 'style']
    preserve_regex = '|'.join(preserve_tags)

    # Protect preserve blocks with placeholders
    preserve_blocks = []
    def preserve(match):
        preserve_blocks.append(match.group(0))
        return f"__PRESERVE_BLOCK_{len(preserve_blocks)-1}__"

    html = re.sub(rf'<({preserve_regex})[\s\S]*?</\1>', preserve, html, flags=re.IGNORECASE)

    # Remove HTML comments
    html = re.sub(r'<!--.*?-->', '', html, flags=re.DOTALL)

    # Collapse all whitespace between tags
    html = re.sub(r'>\s+<', '><', html)

    # Collapse multiple spaces inside tags
    html = re.sub(r'\s+', ' ', html)

    # Restore preserved blocks, stripping JS/CSS comments from script/style
    # (kept verbatim before; comments are pure flash weight once gzipped).
    for i, block in enumerate(preserve_blocks):
        html = html.replace(f"__PRESERVE_BLOCK_{i}__", _process_preserved_block(block))

    return html.strip()

def sanitize_identifier(name: str) -> str:
    """Sanitize a filename to create a valid C identifier.

    C identifiers must:
    - Start with a letter or underscore
    - Contain only letters, digits, and underscores
    """
    # Replace non-alphanumeric characters (including hyphens) with underscores
    sanitized = re.sub(r'[^a-zA-Z0-9_]', '_', name)
    # Prefix with underscore if starts with a digit
    if sanitized and sanitized[0].isdigit():
        sanitized = f"_{sanitized}"
    return sanitized

for root, _, files in os.walk(SRC_DIR):
    for file in files:
        if file.endswith(".html") or file.endswith(".js"):
            file_path = os.path.join(root, file)
            with open(file_path, "r", encoding="utf-8") as f:
                content = f.read()

            # Only minify HTML files; JS files are typically pre-minified (e.g., jszip.min.js)
            if file.endswith(".html"):
                processed = minify_html(content)
            else:
                processed = content

            # Compress with gzip (compresslevel 9 is maximum compression)
            # IMPORTANT: we don't use brotli because Firefox doesn't support brotli with insecured context (only supported on HTTPS)
            compressed = gzip.compress(processed.encode('utf-8'), compresslevel=9)

            # Create valid C identifier from filename
            # Use appropriate suffix based on file type
            suffix = "Html" if file.endswith(".html") else "Js"
            base_name = sanitize_identifier(f"{os.path.splitext(file)[0]}{suffix}")
            header_path = os.path.join(root, f"{base_name}.generated.h")

            with open(header_path, "w", encoding="utf-8") as h:
                h.write(f"// THIS FILE IS AUTOGENERATED, DO NOT EDIT MANUALLY\n\n")
                h.write(f"#pragma once\n")
                h.write(f"#include <cstddef>\n\n")

                # Write the compressed data as a byte array
                h.write(f"constexpr char {base_name}[] PROGMEM = {{\n")

                # Write bytes in rows of 16
                for i in range(0, len(compressed), 16):
                    chunk = compressed[i:i+16]
                    hex_values = ', '.join(f'0x{b:02x}' for b in chunk)
                    h.write(f"  {hex_values},\n")

                h.write(f"}};\n\n")
                h.write(f"constexpr size_t {base_name}CompressedSize = {len(compressed)};\n")
                h.write(f"constexpr size_t {base_name}OriginalSize = {len(processed)};\n")

            print(f"Generated: {header_path}")
            print(f"  Original: {len(content)} bytes")
            print(f"  Minified: {len(processed)} bytes ({100*len(processed)/len(content):.1f}%)")
            print(f"  Compressed: {len(compressed)} bytes ({100*len(compressed)/len(content):.1f}%)")
