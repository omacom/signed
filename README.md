# Signed

![Signed with a signature, a date, and text placed on a lease](docs/screenshot.png)

Sign PDFs and fill them in. Open a PDF, drop your signature, a line of text, or today's date anywhere on a page, move and size them, and save a signed copy, optionally sealed with a digital certificate so any later change to it shows. Modeled on the signing tools in Apple's Preview, and nothing more. On Omarchy, the interface follows your theme's accent color.

Built with **Qt Quick (QML)** and the Material style, **poppler** to show pages, and **qpdf** to write the signed copy. The C++ side compiles to a single executable with the QML embedded.

## Signing

- **Signature** (or *S*) places your last-used signature under the mouse pointer, or in the middle of the page you're looking at when the pointer isn't over a page. The menu next to it lists every saved signature by name.
- **Text** (or *T*) places a text box starting at the mouse pointer and starts editing it. *Shift+Enter* starts a new line; *Enter* or *Esc* finishes. A text box left empty disappears.
- **Date** (or *D*) places today's date in your locale's format, as ordinary text you can edit.
- **Drag** anything to move it, and **drag its corner** to scale it. Signatures and text remember the size you last gave them.
- **Double-click** text to edit it again.
- **Save signed copy…** (*Ctrl+S*) writes a new PDF beside the original, named `<name>-signed.pdf` by default.

The original PDF is never changed, and Signed won't save over it. The signed copy keeps the original pages exactly as they were — text stays selectable, nothing is re-rendered — with your signatures and text laid on top. Placed items stay editable until you close the PDF, so you can fix something and save again.

## Signatures

Choose **Add signature…** in the Signature menu, or drop an image on the window. Use a scan or a well-lit photo of your signature on white paper: Signed removes the paper, keeps the ink (blue stays blue), and crops to it. Slide **Paper threshold** toward *More* if shadows show up, or toward *Less* if faint strokes disappear. PNGs that are already transparent are used as they are. Give it a name like "David" or "Initials".

Signatures are saved as PNGs in `~/.local/share/signed/signatures/` (under `$XDG_DATA_HOME` if set), named after the signature. Deleting one from the menu doesn't affect anything already placed.

## Digital seal

A signature can also seal the copies you save with it. Sealing is off by default; turn it on when you add the signature, under **Digital seal**:

- **Create a certificate** makes a new one for that signature, named after it, with an optional organization.
- An existing certificate can be picked from the same menu, so several signatures can share one.
- **Import a .p12 file…** brings in a certificate from elsewhere. Its password is asked once for the import and never stored.

A sealed copy carries an invisible PKCS#7 signature over the whole document, the kind every PDF viewer understands, so any change made after you saved it shows up there. Copies saved with an unsealed signature are saved as before.

The certificates are self-signed, so a seal proves the file wasn't changed, not who signed it, and some PDF viewers say its signer is unknown. They're stored without a password in `~/.local/share/signed/certificates/`, which is created only once you make or import one: sealing never prompts, but anyone using this computer can seal with them.

## Use Signed from the command line

Signed's commands need no display, so a script or an AI agent can sign a PDF from start to finish with your saved signatures.

```sh
signed signatures                                  # the saved signatures, by name
signed add-signature "Initials" photo.jpg          # save one from a scan or photo
signed pages lease.pdf                             # page sizes in points
signed text lease.pdf --page 4                     # each line of text, with its box
signed render lease.pdf --page 4 --grid -o p4.png  # look at a page, with a 50-point grid
signed sign lease.pdf lease-signed.pdf --items items.json
```

`sign` places each item described in `items.json`: a signature by name, text, or today's date, on a page, at a position in points from the top-left corner. For example, `{"signature": "Alex Morgan", "page": 4, "x": 76, "bottom": 198, "width": 150}`. It checks every item before writing anything, reports all the problems at once, and never writes over the original. `signed help items` prints the whole format. Add `--json` to any command for structured output. Commands exit 0 on success and 1 on failure, with errors on stderr.

`signed` alone lists the commands, and `signed open [pdf]` starts the app. `signed help <command>` lists a command's options.

To teach your coding agents about Signed, run `signed skill install`. It copies a short skill to `~/.agents/skills/signed/`, where Codex finds it, and links it into `~/.claude/skills/` for Claude Code. `signed skill` prints it instead.

## Hotkeys

| Key | Action |
| --- | --- |
| *Ctrl+O* | Open a PDF |
| *Ctrl+S* | Save a signed copy |
| *S* | Place the last-used signature |
| *Shift+S* | Open the signature menu |
| *T* | Place text |
| *D* | Place today's date |
| *Enter* | Edit the selected text |
| *Arrows* / *Shift+Arrows* | Nudge the selection 1 pt / 10 pt |
| *[* / *]* | Make the selection smaller / larger |
| *Ctrl+D* | Duplicate the selection |
| *Delete*, *Backspace*, *X* | Remove the selection |
| *Esc* | Deselect / stop editing |
| *Page Up* / *Page Down* | Previous / next page |
| *Alt+Page Up* / *Alt+Page Down* | Move the selection onto the previous / next page |
| *Ctrl+=* / *Ctrl+-* / *Ctrl+0* | Zoom in / out / fit (or *Ctrl+scroll*) |
| *Ctrl+Z* / *Ctrl+Shift+Z* | Undo / redo |
| *Q* | Quit (asks first if there's anything unsaved) |
| *?* | Show the hotkeys in the app |

Letter keys do nothing while you're typing.

## Limits

- Password-protected PDFs can't be opened.
- Form fields aren't filled in as fields; place text over them instead.
- Signatures come from images; there's no drawing with the mouse.
- A digital seal is self-signed: it shows the file is unchanged, but it isn't a qualified electronic signature (QES).

## Requirements

- `poppler-qt6` and `qpdf`
- `nss` (certificate database and creation; poppler pulls it in anyway)
- `noto-fonts` (text is set in Noto Sans and embedded in the PDF)
- `xdg-desktop-portal` and a portal backend for the file picker

## Build

Uses Qt's own build tool, `qmake6`:

```bash
./bin/build
./build/signed open some.pdf
```

Run the tests (offscreen, including the command line) with `./bin/test`, and install as an Arch package with `./bin/install`.

## License

MIT
