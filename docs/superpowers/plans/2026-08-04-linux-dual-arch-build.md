# Linux Dual-Architecture Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a GitHub Actions workflow that builds and uploads deployable ZIP packages for RHEL 7.9 x86_64 and Kylin V10 aarch64.

**Architecture:** A single matrix workflow runs each build on its native GitHub-hosted Ubuntu runner. JavaScript actions run on that host; a `docker run` step mounts the workspace into the version-specific ABI-baseline image for compilation, staging, dependency bundling, ELF/GLIBC auditing, and ZIP creation. This avoids executing the Actions Node runtime in the RHEL 7 glibc 2.17 image.

**Tech Stack:** GitHub Actions YAML, GitHub-hosted Ubuntu runners, RHEL-compatible containers, Autotools, GNU Make, `file`, `readelf`, `zip`.

## Global Constraints

- x86_64 runs its compilation in a RHEL 7.9 ABI-compatible Docker image and must assert GCC/G++ 4.8.5; it must not install a newer toolset.
- aarch64 runs natively on `ubuntu-24.04-arm` and uses a RHEL 8 ABI-compatible Docker image to target Kylin V10 glibc 2.28.
- Build through `./configure`, `make`, and `make install`; do not alter C/C++ code, Autotools inputs, CMake, XML, or runtime configuration.
- Install with `--prefix=/opt/tte-mrcp` and `DESTDIR="$GITHUB_WORKSPACE/package-root"`; ZIP content root is `opt/tte-mrcp/`.
- ZIP names are exactly `tte-mrcp-rhel7-x86_64.zip` and `tte-mrcp-kylinv10-aarch64.zip`.
- Every ZIP contains `bin/unimrcpserver`, `bin/umc`, `bin/unimrcpclient`, `bin/asrclient`, plus `lib/`, `plugin/`, `conf/`, and `data/`.
- The workflow uploads artifacts only; it must not create Releases, tags, deployments, or live MRCP/TTS requests.

---

### Task 1: Add the native, ABI-baselined build workflow

**Files:**
- Create: `.github/workflows/build-linux.yml`

**Interfaces:**
- Consumes: GitHub `pull_request`, `push`, and `workflow_dispatch` events; repository source tree; public build-container images.
- Produces: One ZIP artifact per matrix row, with names and paths supplied by `matrix.artifact_name` and `matrix.zip_name`.

- [x] **Step 1: Create the workflow triggers and two-row matrix**

```yaml
on:
  pull_request:
    branches: [main]
  push:
    branches: [main]
  workflow_dispatch:

strategy:
  fail-fast: false
  matrix:
    include:
      - target: rhel7-x86_64
        runner: ubuntu-24.04
        build_image: quay.io/centos/centos:7
        elf_machine: Advanced Micro Devices X86-64
        glibc_max: 2.17
        require_gcc_485: true
        artifact_name: tte-mrcp-rhel7-x86_64
        zip_name: tte-mrcp-rhel7-x86_64.zip
      - target: kylinv10-aarch64
        runner: ubuntu-24.04-arm
        build_image: rockylinux:8
        elf_machine: AArch64
        glibc_max: 2.28
        require_gcc_485: false
        artifact_name: tte-mrcp-kylinv10-aarch64
        zip_name: tte-mrcp-kylinv10-aarch64.zip
```

- [x] **Step 2: Check out on the native runner and run the following build steps through `docker run --rm --volume "$GITHUB_WORKSPACE:/work" --workdir /work "${{ matrix.build_image }}" bash -ceu '<script>'`**

The system repositories on the aarch64 Rocky Linux 8 baseline do not provide
`sofia-sip-devel`. The workflow therefore installs the common Autotools tools
and builds Sofia-SIP from the fixed `v1.13.17` tag, asserting commit
`6198851a610b7889c17e2d98fb84617bc1dd7aec`, before configuring TTE-MRCP.

```bash
if [ "${{ matrix.target }}" = "rhel7-x86_64" ]; then
  sed -i \
    -e 's|^mirrorlist=|#mirrorlist=|g' \
    -e 's|^#baseurl=http://mirror.centos.org|baseurl=https://vault.centos.org|g' \
    /etc/yum.repos.d/CentOS-*.repo
  yum -y install apr-devel apr-util-devel gcc gcc-c++ make openssl-devel pkgconfig autoconf automake diffutils git libtool unzip zip
else
  dnf -y install apr-devel apr-util-devel gcc gcc-c++ make openssl-devel pkgconf-pkg-config autoconf automake diffutils git libtool unzip zip
fi

git clone --depth 1 --branch v1.13.17 https://github.com/freeswitch/sofia-sip.git /tmp/sofia-sip
test "$(cd /tmp/sofia-sip && git rev-parse HEAD)" = 6198851a610b7889c17e2d98fb84617bc1dd7aec
cd /tmp/sofia-sip && ./bootstrap.sh && ./configure --prefix=/opt/tte-mrcp && make && make install
export PKG_CONFIG_PATH=/opt/tte-mrcp/lib/pkgconfig
```

- [x] **Step 3: Assert the compiler and configure the installation staging tree**

```bash
gcc --version
g++ --version
if [ "${{ matrix.require_gcc_485 }}" = "true" ]; then
  test "$(gcc -dumpfullversion -dumpversion)" = "4.8.5"
  test "$(g++ -dumpfullversion -dumpversion)" = "4.8.5"
fi

rm -rf build/ci package-root
mkdir -p build/ci
cd build/ci
../../configure --prefix=/opt/tte-mrcp --with-sofia-sip=/opt/tte-mrcp
# aarch64 matrix only:
# ../../configure --prefix=/opt/tte-mrcp --with-sofia-sip=/opt/tte-mrcp --build=aarch64-unknown-linux-gnu
make -j"$(getconf _NPROCESSORS_ONLN)"
make install DESTDIR="$GITHUB_WORKSPACE/package-root"
```

- [x] **Step 4: Verify package layout, ELF architecture, and GLIBC ceiling**

```bash
package_dir="$GITHUB_WORKSPACE/package-root/opt/tte-mrcp"
test -x "$package_dir/bin/unimrcpserver"
test -x "$package_dir/bin/umc"
test -x "$package_dir/bin/unimrcpclient"
test -x "$package_dir/bin/asrclient"
test -d "$package_dir/lib"
test -d "$package_dir/plugin"
test -d "$package_dir/conf"
test -d "$package_dir/data"

find "$package_dir" -type f -exec sh -c '
  file "$1" | grep -q ELF || exit 0
  file "$1" | grep -Fq "${{ matrix.elf_machine }}"
  readelf --version-info "$1" | awk -v maximum="${{ matrix.glibc_max }}" "/GLIBC_[0-9]/ { sub(/^.*GLIBC_/, \"\", \$0); split(\$0, v, /[^0-9.]/); if (v[1] > maximum) exit 1 }"
' sh {} \;
```

- [x] **Step 5: Bundle non-system shared-library dependencies, build the named ZIP, and upload only the artifact**

```bash
export LD_LIBRARY_PATH="$package_dir/lib"
find "$package_dir" -type f -exec sh -c '
  file "$1" | grep -q ELF || exit 0
  ldd "$1" | awk "/=> \\// {print \\$3} /^\\// {print \\$1}"
' sh {} \; | sort -u | while read -r library; do
  case "$library" in
    /lib*/ld-linux*|/lib*/libc.so.*|/lib*/libdl.so.*|/lib*/libm.so.*|/lib*/libpthread.so.*|/lib*/librt.so.*)
      ;;
    *)
      cp -a "$library" "$package_dir/lib/"
      ;;
  esac
done

cd "$GITHUB_WORKSPACE/package-root"
zip -r "$GITHUB_WORKSPACE/${{ matrix.zip_name }}" opt/tte-mrcp
unzip -l "$GITHUB_WORKSPACE/${{ matrix.zip_name }}"
```

```yaml
- uses: actions/upload-artifact@v4
  with:
    name: ${{ matrix.artifact_name }}
    path: ${{ matrix.zip_name }}
    if-no-files-found: error
```

- [x] **Step 6: Validate the workflow contract locally**

Run: `ruby -e 'require "yaml"; YAML.load_file(".github/workflows/build-linux.yml")'`

Expected: exit status 0.

Run: `rg -n 'ubuntu-24\.04-arm|quay\.io/centos/centos:7|rockylinux:8|4\.8\.5|tte-mrcp-rhel7-x86_64\.zip|tte-mrcp-kylinv10-aarch64\.zip' .github/workflows/build-linux.yml`

Expected: each required runner, container, compiler floor, and ZIP name is present.

- [x] **Step 7: Check final whitespace and scope**

Run: `git diff --check -- .github/workflows/build-linux.yml`

Expected: exit status 0 with no output.

Run: `git status --short`

Expected: only the workflow and the user-owned pre-existing design/plan documents are untracked or modified by this task.

### Task 2: Refresh the repository knowledge graph after workflow creation

**Files:**
- Modify: `.codebase-memory/graph.db.zst` only if the indexer writes a persisted graph artifact.

**Interfaces:**
- Consumes: final active repository files, including `.github/workflows/build-linux.yml`.
- Produces: fresh index result and canonical workflow-file discovery evidence for the delivery record.

- [x] **Step 1: Run a fast repository index**

Run: `index_repository(repo_path="/Users/huangzhonghui/tte-mrcp", mode="fast")`

Expected: indexing completes without an extraction error.

- [x] **Step 2: Query the active workflow file**

Run: `search_code(project="tte-mrcp", pattern="tte-mrcp-kylinv10-aarch64.zip", path_filter="^\\.github/workflows/", mode="files")`

Expected: `.github/workflows/build-linux.yml` is returned as the active workflow file.

- [x] **Step 3: Record scope and verification status in the handoff**

Run: `git diff --check`

Expected: exit status 0 with no output; report that GitHub-hosted runners have not executed the new workflow until the branch is pushed and a run completes.
