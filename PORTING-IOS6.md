# This fork: WebKit for armv7 / iOS 6.1.3

This is WebKit, forked to run on an iPhone 4S. Everything specific to that lives
behind one macro, `WEBKIT_IOS6`, so the diff against upstream reads as a set of
answers to a single question: what does this code assume about the system that a
2013 release does not provide?

The engine is built by CMake from the repository that carries the toolchain, the
dependencies and the device-side pieces:
**[revenant-webkit](https://github.com/kern0x1b/revenant-webkit)**. This fork is
the engine half; that repository is the other half, and its README is where the
build and deploy instructions are.

## What the port has to deal with

- **The system's own API is a decade older than the engine's assumptions.** Where
  a call arrived after iOS 6, the port either takes a different route WebKit
  already has - a software path, a pre-2015 implementation from WebKit's own
  history, an older CoreText or CoreGraphics entry point - or, where the platform
  genuinely cannot answer, it says so rather than answering wrongly. The
  compatibility layer that stands in for missing symbols lives in the other
  repository, under `compat/`, and is audited in `STUB-AUDIT.md`.
- **512 MB of RAM, and the system takes the process away at about 280 MB.** A
  good part of the diff is budgets: caches, collector bands, tile and layer
  limits. Every one of them carries the measurement that set it, in the comment
  next to it - and where a number was picked without one, the comment says that
  too.
- **One core pair, and no GPU process.** This is WebKit1: the engine runs in the
  application. There is no compositing process, no GPU process, and no IOSurface.

## Reading the diff

- `#if defined(WEBKIT_IOS6)` is the port. Upstream behaviour is always the other
  branch, never deleted.
- Comments carry the reason and, where there is one, the number. A change with no
  reason next to it is a bug, not a convention.
- Commit subjects say what changed for the reader of a page, not which function
  was edited.

## Branch

Port work lands on `ios6-armv7`. Upstream refs are kept as they came, for the
history searches this work depends on: the port branch is a shallow graft, so a
`git log -S` for anything historical has to run against a full upstream ref
(`origin/webkitglib/2.54` is the same era as this tree).

## Licence

WebKit's own, unchanged. See `ReadMe.md` and the per-file headers.
