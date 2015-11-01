#!/bin/sh

set -eux

gen_uml() {

	local file="${1}"
	local script="${2}"

	cat > "${file}" <<-EOF
		#!/bin/sh

		set -eux

		PATH="${PATH}:$(ls /usr/libexec/gcc/*-pc-linux-gnu/*.*)"
		export

		insmod "/usr/lib/uml/modules/\`uname -r\`/kernel/fs/fuse/fuse.ko"

		cd "${PWD}"

		set +e
		"${script}"
		err=\$?
		set -e

		echo \${err} > "${PWD}/umltest.status"

		halt -f
	EOF
	chmod +x "${file}"
}

run_uml() {
	local file="${1}"

	/usr/bin/linux.uml init="${file}" rootfstype=hostfs rw
}

launch_uml() {

	local script="${1}"

	local file="$(mktemp)"

	gen_uml "${file}" "${script}"
	run_uml "${file}"
}

case "${1}" in
	install) launch_uml './bootstrap.sh';;
	script)  cd test; launch_uml './test.sh';;
esac

exit $(cat umltest.status)
