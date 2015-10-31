#!/bin/sh

set +x

su <<-EOF
	./bootstrap.sh
	cd test && ./test.sh
EOF
