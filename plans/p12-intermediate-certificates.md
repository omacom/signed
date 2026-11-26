# Imported intermediate certificates

When a `.p12` is imported, Signed keeps the non-root intermediate certificates
that come with it and includes them in every copy sealed with that certificate.
A viewer that has only the root certificate can then validate the copy.

## Behaviour

- If the `.p12` contains non-root intermediate certificates, they are included
  in the digital seal of every copy sealed with that certificate.
- If it contains none, only the signing certificate is included. Importing and
  sealing still succeed; no warning or prompt is shown.
- Self-signed roots are not added to seals. They are trust anchors, not
  intermediates.
- Certificates Signed creates itself are self-signed and have no
  intermediates. Their behaviour is unchanged.
- Selecting the certificate and sealing work as before. There is no new chain
  screen or approval step.

## Why

A PDF viewer often has a store of root certificates but not the intermediate
issuers. If the seal carries the intermediates, the viewer can build the path
from the signing certificate to a root it trusts. Without them, the viewer may
report the signer as unknown even though the root is present.

## Scope

- Applies to certificates imported from `.p12` files.
- Only non-root intermediates on the signing certificate's chain are included.
- A `.p12` without intermediates is still valid. It simply relies on the viewer
  already having the intermediate, or on the seal being accepted without one.
- Re-importing a `.p12` refreshes what is available for that certificate.

## Validation

- A `.p12` with a root, one or more intermediates, and a leaf produces a seal
  that validates on a viewer that has only the root.
- A `.p12` with no intermediates produces a seal containing the leaf.
  Sealing is not blocked and the user is not asked to supply a chain.
