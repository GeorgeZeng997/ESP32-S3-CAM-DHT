#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace camsec {

// Validate a single path segment (e.g. run directory name or filename) to prevent SD path traversal.
// Allows only [A-Za-z0-9._-], length 1..64, and rejects "." or "..".
inline bool isValidSafeSegment(const char *seg) {
  if (seg == nullptr || seg[0] == '\0') {
    return false;
  }
  if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) {
    return false;
  }
  size_t len = 0;
  for (; seg[len] != '\0'; ++len) {
    if (len >= 64) {
      return false;
    }
    const char c = seg[len];
    const bool ok = (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.';
    if (!ok) {
      return false;
    }
  }
  return len > 0;
}

// Constant-time string comparison for bearer / API tokens.
inline bool constantTimeEquals(const char *a, const char *b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  const size_t lenA = strlen(a);
  const size_t lenB = strlen(b);
  uint8_t diff = static_cast<uint8_t>(lenA ^ lenB);
  const size_t n = (lenA < lenB) ? lenA : lenB;
  for (size_t i = 0; i < n; ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

// Escape untrusted text for safe insertion inside HTML attributes / elements.
inline size_t htmlEscapeToBuf(const char *src, char *dst, size_t dstSize) {
  if (dst == nullptr || dstSize == 0) return 0;
  dst[0] = '\0';
  if (src == nullptr) return 0;

  size_t out = 0;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    const char *rep = nullptr;
    size_t repLen = 0;
    switch (src[i]) {
      case '&':  rep = "&amp;";  repLen = 5; break;
      case '<':  rep = "&lt;";   repLen = 4; break;
      case '>':  rep = "&gt;";   repLen = 4; break;
      case '"':  rep = "&quot;"; repLen = 6; break;
      case '\'': rep = "&#39;";  repLen = 5; break;
      default: break;
    }
    if (rep != nullptr) {
      if (out + repLen >= dstSize) break;
      memcpy(dst + out, rep, repLen);
      out += repLen;
    } else {
      if (out + 1 >= dstSize) break;
      const unsigned char ch = static_cast<unsigned char>(src[i]);
      dst[out++] = (ch < 0x20U || ch == 0x7FU) ? ' ' : src[i];
    }
  }
  dst[out] = '\0';
  return out;
}

// Infer HTTP Content-Type from filename extension.
inline const char *contentTypeForFilename(const char *file) {
  if (file == nullptr) return "application/octet-stream";
  const char *dot = strrchr(file, '.');
  if (dot == nullptr) return "application/octet-stream";
  if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".JPG") == 0 ||
      strcmp(dot, ".jpeg") == 0 || strcmp(dot, ".JPEG") == 0) {
    return "image/jpeg";
  }
  if (strcmp(dot, ".csv") == 0 || strcmp(dot, ".CSV") == 0) {
    return "text/csv; charset=utf-8";
  }
  return "application/octet-stream";
}

inline int clampPage(int page) {
  return (page < 1) ? 1 : ((page > 100000) ? 100000 : page);
}

inline int clampPageSize(int pageSize) {
  return (pageSize < 1) ? 1 : ((pageSize > 100) ? 100 : pageSize);
}

}  // namespace camsec
