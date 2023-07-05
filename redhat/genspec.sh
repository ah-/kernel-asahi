#!/bin/bash

LAST_MARKER=$(cat "${REDHAT}"/marker)
clogf="$SOURCES/changelog"
# hide [redhat] entries from changelog
HIDE_REDHAT=1;
# hide entries for unsupported arches
HIDE_UNSUPPORTED_ARCH=1;
# override LC_TIME to avoid date conflicts when building the srpm
LC_TIME=
# STAMP=$(echo $MARKER | cut -f 1 -d '-' | sed -e "s/v//"); # unused

UPSTREAM=$(git rev-parse -q --verify origin/"${UPSTREAM_BRANCH}" || \
	   git rev-parse -q --verify "${UPSTREAM_BRANCH}")

if [ "$SNAPSHOT" = 0 ]; then
	# This is based off a tag on Linus's tree (e.g. v5.5 or v5.5-rc5).
	# Two kernels are built, one with debug configuration and one without.
	DEBUG_BUILDS_ENABLED=1
else
	# All kernels are built with debug configurations.
	DEBUG_BUILDS_ENABLED=0
fi

if [ -n "$BUILDID" ]; then
	BUILDID_DEFINE=$(printf "%%define buildid %s" "$BUILDID")
else
	BUILDID_DEFINE="# define buildid .local"
fi

EXCLUDE_FILES=":(exclude,top).get_maintainer.conf \
		:(exclude,top).gitattributes \
		:(exclude,top).gitignore \
		:(exclude,top).gitlab-ci.yml \
		:(exclude,top)makefile \
		:(exclude,top)Makefile.rhelver \
		:(exclude,top)redhat \
		:(exclude,top)configs"

# If PATCHLIST_URL is not set to "none", generate Patchlist.changelog file that
# holds the shas and commits not included upstream and git commit url.
PATCHLIST_CHANGELOG=0
if [ "$PATCHLIST_URL" != "none" ]; then
	# sed convert
	# <sha> <description>
	# to
	# <ark_commit_url>/<sha>
	#  <sha> <description>
	#
	# May need to preserve word splitting in EXCLUDE_FILES
	# shellcheck disable=SC2086
	git log --no-merges --pretty=oneline --no-decorate ${UPSTREAM}.. $EXCLUDE_FILES | \
		sed "s!^\([^ ]*\)!$PATCHLIST_URL/\1\n &!; s!\$!\n!" \
		> "$SOURCES"/Patchlist.changelog
	PATCHLIST_CHANGELOG=1
fi

test -f "$SOURCES/$SPECFILE" &&
	sed -i -e "
	/%%CHANGELOG%%/r $SOURCES/$CHANGELOG
	/%%CHANGELOG%%/d
	s/%%BUILDID%%/$BUILDID_DEFINE/
	s/%%RPMKVERSION%%/$RPMKVERSION/
	s/%%RPMKPATCHLEVEL%%/$RPMKPATCHLEVEL/
	s/%%RPMKSUBLEVEL%%/$RPMKSUBLEVEL/
	s/%%PKGRELEASE%%/$PKGRELEASE/
	s/%%SPECRELEASE%%/$SPECRELEASE/
	s/%%DISTRO_BUILD%%/$DISTRO_BUILD/
	s/%%RELEASED_KERNEL%%/$RELEASED_KERNEL/
	s/%%DEBUG_BUILDS_ENABLED%%/$DEBUG_BUILDS_ENABLED/
	s/%%INCLUDE_FEDORA_FILES%%/$INCLUDE_FEDORA_FILES/
	s/%%INCLUDE_RHEL_FILES%%/$INCLUDE_RHEL_FILES/
	s/%%PATCHLIST_CHANGELOG%%/$PATCHLIST_CHANGELOG/
	s/%%TARFILE_RELEASE%%/$TARFILE_RELEASE/" "$SOURCES/$SPECFILE"

# We depend on work splitting of BUILDOPTS
# shellcheck disable=SC2086
for opt in $BUILDOPTS; do
	add_opt=
	[ -z "${opt##+*}" ] && add_opt="_with_${opt#?}"
	[ -z "${opt##-*}" ] && add_opt="_without_${opt#?}"
	[ -n "$add_opt" ] && sed -i "s/^\\(# The following build options\\)/%define $add_opt 1\\n\\1/" "$SOURCES/$SPECFILE"
done

# The self-test data doesn't currently have tests for the changelog or patch file, so the
# rest of the script can be ignored.  See redhat/Makefile setup-source target for related
# test changes.
[ -n "$RHSELFTESTDATA" ] && exit 0

echo > "$clogf"

lasttag=$(git rev-list --first-parent --grep="^\[redhat\] kernel-${RPMKVERSION}.${RPMKPATCHLEVEL}" --max-count=1 HEAD)
# if we didn't find the proper tag, assume this is the first release
if [[ -z $lasttag ]]; then
    if [[ -z ${MARKER//[0-9a-f]/} ]]; then
        # if we're doing an untagged release, just use the marker
        echo "Using $MARKER"
        lasttag=$MARKER
    else
	lasttag=$(git describe --match="$MARKER" --abbrev=0)
    fi
fi
echo "Gathering new log entries since $lasttag"
# master is expected to track mainline.

git log --topo-order --reverse --no-merges -z --format="- %s (%an)%n%b" \
	^"${UPSTREAM}" "$lasttag".. -- ':!/redhat/rhdocs' | "${0%/*}"/genlog.py >> "$clogf"

grep -v "tagging $RPMVERSION" "$clogf" > "$clogf.stripped"
cp "$clogf.stripped" "$clogf"

if [ "$HIDE_REDHAT" = "1" ]; then
	grep -v -e "^- \[redhat\]" "$clogf" |
		sed -e 's!\[Fedora\]!!g' > "$clogf.stripped"
	cp "$clogf.stripped" "$clogf"
fi

if [ "$HIDE_UNSUPPORTED_ARCH" = "1" ]; then
	grep -E -v "^- \[(alpha|arc|arm|avr32|blackfin|c6x|cris|frv|h8300|hexagon|ia64|m32r|m68k|metag|microblaze|mips|mn10300|openrisc|parisc|score|sh|sparc|tile|um|unicore32|xtensa)\]" "$clogf" > "$clogf.stripped"
	cp "$clogf.stripped" "$clogf"
fi

# If the markers aren't the same then this a rebase.
# This means we need to zap entries that are already present in the changelog.
if [ "$MARKER" != "$LAST_MARKER" ]; then
	# awk trick to get all unique lines
	awk '!seen[$0]++' "$SOURCES/$CHANGELOG" "$clogf" > "$clogf.unique"
	# sed trick to get the end of the changelog minus the line
	sed -e '1,/# END OF CHANGELOG/ d' "$clogf.unique" > "$clogf.tmp"
	# Add an explicit entry to indicate a rebase.
	echo "" > "$clogf"
	echo -e "- $MARKER rebase" | cat "$clogf.tmp" - >> "$clogf"
	rm "$clogf.tmp" "$clogf.unique"
fi

# HACK temporary hack until single tree workflow
# Don't reprint all the ark-patches again.
if [ -n "$(git log --oneline --first-parent --grep="Merge ark patches" "$lasttag"..)" ]; then
	# Throw away the clogf and just print the summary merge
	echo "" > "$clogf"
	echo "- Merge ark-patches" >> "$clogf"
fi

LENGTH=$(wc -l "$clogf" | awk '{print $1}')

#the changelog was created in reverse order
#also remove the blank on top, if it exists
#left by the 'print version\n' logic above
cname="$(git var GIT_COMMITTER_IDENT |sed 's/>.*/>/')"
cdate="$(LC_ALL=C date +"%a %b %d %Y")"
cversion="[$RPMVERSION]";
tac "$clogf" | sed "1{/^$/d; /^- /i\
* $cdate $cname $cversion
	}" > "$clogf.rev"

if [ "$LENGTH" = 0 ]; then
	rm -f "$clogf.rev"; touch "$clogf.rev"
fi

cat "$clogf.rev" "$SOURCES/$CHANGELOG" > "$clogf.full"
mv -f "$clogf.full" "$SOURCES/$CHANGELOG"

echo "MARKER is $MARKER"

if [ "$SINGLE_TARBALL" = 0 ]; then
	# May need to preserve word splitting in EXCLUDE_FILES
	# shellcheck disable=SC2086
	git diff -p --no-renames --stat "$MARKER"..  $EXCLUDE_FILES \
		> ${SOURCES}/patch-${RPMKVERSION}.${RPMKPATCHLEVEL}-redhat.patch
else
	# Need an empty file for dist-git compatibility
	touch "${SOURCES}/patch-${RPMKVERSION}.${RPMKPATCHLEVEL}"-redhat.patch
fi

rm -f "$clogf"{,.rev,.stripped};
