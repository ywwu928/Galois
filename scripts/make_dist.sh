#!/bin/bash
#
# Make distribution tarball

NAME="Galois-6.0.0"

if [[ ! -e COPYRIGHT ]]; then
  echo "Run this from the root source directory" 1>&2
  exit 1
fi

touch "$NAME.tar.gz" # Prevent . from changing during tar
#(svn status | grep '^\?' | sed -e 's/^\? *//'; \
( \
  echo ".git"; \
  echo "*.swp"; \
  echo "*~"; \
  echo "exp"; \
  echo "$NAME.tar.gz") | \
  tar --exclude-from=- --exclude-vcs --transform "s,^\./,$NAME/," -cz -f "$NAME.tar.gz" .
