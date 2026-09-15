Drop extra CA certificates (`*.crt`, PEM format) in this directory to build
behind a TLS-inspecting proxy. Anything here is installed into the image's
trust store before any network access happens.

Certificates are deliberately not committed: add them locally and they will be
picked up, or leave the directory empty and nothing changes.
