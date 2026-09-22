#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""pkg_test.py - HW round-trip for `pkg` (B1 packaged SD apps).

Precondition: `python apps/push_pkgs.py COMx` has pushed PKGS/PKGTEST.
Exercises list/info/verify/install/run/remove and asserts the installed files
appear and disappear. Then a self-contained ECDSA matrix: fresh keypair per
run, `pkg key` trust store, signed install OK, flipped-signature refusal,
unsigned policy notes, and key cleanup.
"""
import hashlib
import os
import sys
import time
import zlib

sys.path.insert(0, "tools")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "apps", "companion"))
from bg_run import boot, run_quiet  # noqa: E402
from push_sd import push_file, read_until  # noqa: E402

FAILS = []


def check(name, out, needle, want=True):
    got = needle in out
    ok = got == want
    print("  [%s] %s (%s%r)" % ("PASS" if ok else "FAIL", name,
                                "" if want else "not ", needle))
    if not ok:
        FAILS.append(name)
        print("      out: %r" % out[-400:])


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    try:
        run_quiet(sh, "pkg remove PKGTEST")  # clean slate

        print("-- preconditions")
        check("not listed", run_quiet(sh, "pkg list"), "PKGTEST", want=False)
        check("no manifest", run_quiet(sh, "pkg verify PKGTEST"),
              "no manifest", want=True)

        print("-- install")
        out = run_quiet(sh, "pkg install PKGTEST")
        check("payload copied", out, "pkg: installed PKGTEST.BAT")
        check("installed", out, "pkg: PKGTEST installed")

        print("-- installed state")
        check("listed", run_quiet(sh, "pkg list"), "PKGTEST")
        check("verify ok", run_quiet(sh, "pkg verify PKGTEST"), "1/1 ok")
        info = run_quiet(sh, "pkg info PKGTEST")
        check("title", info, "Package Test")
        check("version", info, "1.0")

        print("-- runs")
        run = run_quiet(sh, "PKGTEST.BAT")
        check("marker run", run, "[M-PKGTEST]")
        check("marker done", run, "[M-PKGTEST-DONE]")

        print("-- check all")
        check("check sees it", run_quiet(sh, "pkg check", timeout=45), "PKGTEST")

        print("-- remove")
        out = run_quiet(sh, "pkg remove PKGTEST")
        check("uninstalled", out, "uninstalled")
        check("not listed after", run_quiet(sh, "pkg list"), "PKGTEST", want=False)
        check("no manifest after", run_quiet(sh, "pkg verify PKGTEST"),
              "no manifest")
        check("no info after", run_quiet(sh, "pkg info PKGTEST"),
              "(missing)")

        print("-- signed manifests (fresh keypair, hermetic)")
        try:
            from cryptography.hazmat.primitives.asymmetric import ec
            from cryptography.hazmat.primitives import hashes
            from cryptography.hazmat.primitives.asymmetric.utils import (
                decode_dss_signature)
        except ImportError:
            print("  [SKIP] cryptography package missing")
        else:
            priv = ec.generate_private_key(ec.SECP256R1())
            nums = priv.public_key().public_numbers()
            pub_raw = nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")

            def read_app(path):
                with open(path, "rb") as f:
                    return f.read()

            def signed_bundle(body):
                canon = b"".join(
                    ln + b"\n" for ln in body.split(b"\n")
                    if ln.strip() and not ln.strip().startswith((b"#", b";")))
                assert b"SIGN=" not in canon
                r, s = decode_dss_signature(
                    priv.sign(canon, ec.ECDSA(hashes.SHA256())))
                return ("SIGN=%064x%064x\n" % (r, s)).encode() + body

            bat = read_app("apps/pkgtest/PKGTEST.BAT")
            ai = read_app("apps/pkgtest/PKGTEST.APPINFO")
            manifest = ("; SIGTEST bundle\nSIGTEST.BAT=%08X\n" %
                        (zlib.crc32(bat) & 0xFFFFFFFF)).encode()
            sigline = signed_bundle(manifest).split(b"\n", 1)[0] + b"\n"

            # Hermetic start: drop any key left by an earlier run (the clear is
            # a destructive confirmation), then assert a clean slate.
            def clear_key():
                sh.s.reset_input_buffer()
                sh.s.write(b"pkg key clear\r\n")
                if b"Type YES" in read_until(sh.s, b"Type YES to continue:", 12):
                    sh.s.write(b"YES\r\n")
                    # Drain the completion line so it cannot leak into the
                    # next command's capture window.
                    read_until(sh.s, b"trusted key cleared", 8)

            clear_key()
            check("no trusted key yet", run_quiet(sh, "pkg key show"),
                  "no trusted key")
            run_quiet(sh, "md PKGS/SIGTEST")
            run_quiet(sh, "md PKGS/SIGTES2")
            sh.s.reset_input_buffer()
            pushed = push_file(sh.s, "PKGS/SIGTEST/SIGTEST.BAT", bat)
            check("push payload", "ok" if pushed else "FAIL", "ok")
            pushed = push_file(sh.s, "PKGS/SIGTEST/SIGTEST.APPINFO", ai)
            check("push appinfo", "ok" if pushed else "FAIL", "ok")
            pushed = push_file(sh.s, "PKGS/SIGTEST/SIGTEST.ASSETS",
                               sigline + manifest)
            check("push signed manifest", "ok" if pushed else "FAIL", "ok")

            out = run_quiet(sh, "pkg install SIGTEST")
            check("signed install warns without key", out, "no trusted key")
            check("signed install still lands", out, "pkg: SIGTEST installed")
            out = run_quiet(sh, "pkg verify SIGTEST")
            check("verify reports key gap", out, "no trusted key")
            run_quiet(sh, "pkg remove SIGTEST")

            sh.s.reset_input_buffer()
            pushed = push_file(sh.s, "SIGPUB.BIN", pub_raw)
            check("push pubkey", "ok" if pushed else "FAIL", "ok")
            out = run_quiet(sh, "pkg key install SIGPUB.BIN")
            check("key installs", out, "trusted key")
            check("key fingerprint", out, hashlib.sha256(pub_raw).hexdigest()[:16])

            out = run_quiet(sh, "pkg install SIGTEST")
            check("signed install verifies", out, "signed OK")
            out = run_quiet(sh, "pkg verify SIGTEST")
            check("verify signed ok", out, "signed OK")
            check("info shows signed", run_quiet(sh, "pkg info SIGTEST"),
                  "signed:      yes")

            # Flip one signature nibble in a correctly-named bundle copy:
            # install must refuse before touching anything.
            bad_manifest = ("; SIGTES2 bundle\nSIGTES2.BAT=%08X\n" %
                            (zlib.crc32(bat) & 0xFFFFFFFF)).encode()
            bad = bytearray(signed_bundle(bad_manifest))
            bad[6] = ord("0") if bad[6:7] != b"0" else ord("1")
            sh.s.reset_input_buffer()
            pushed = push_file(sh.s, "PKGS/SIGTES2/SIGTES2.BAT", bat)
            check("push bad-bundle payload", "ok" if pushed else "FAIL", "ok")
            run_quiet(sh, "del /p PKGS/SIGTES2/SIGTES2.APPINFO")
            pushed = push_file(sh.s, "PKGS/SIGTES2/SIGTES2.APPINFO", ai)
            check("push bad-bundle appinfo", "ok" if pushed else "FAIL", "ok")
            pushed = push_file(sh.s, "PKGS/SIGTES2/SIGTES2.ASSETS", bytes(bad))
            check("push bad-bundle manifest", "ok" if pushed else "FAIL", "ok")
            out = run_quiet(sh, "pkg install SIGTES2")
            check("bad signature refuses", out, "BAD signature")
            check("refused install touches nothing",
                  run_quiet(sh, "pkg list"), "SIGTES2", want=False)
            run_quiet(sh, "del /p PKGS/SIGTES2/SIGTES2.BAT")
            run_quiet(sh, "del /p PKGS/SIGTES2/SIGTES2.APPINFO")
            run_quiet(sh, "del /p PKGS/SIGTES2/SIGTES2.ASSETS")

            # Unsigned policy: note by default, refuse with /signed.
            out = run_quiet(sh, "pkg install PKGTEST")
            check("unsigned install notes CRC-only", out, "unsigned (CRC-only)")
            run_quiet(sh, "pkg remove PKGTEST")
            out = run_quiet(sh, "pkg install PKGTEST /signed")
            check("unsigned refused when enforced", out, "unsigned (policy refuses)")

            run_quiet(sh, "pkg remove SIGTEST")
            run_quiet(sh, "del /p PKGS/SIGTEST/SIGTEST.BAT")
            run_quiet(sh, "del /p PKGS/SIGTEST/SIGTEST.APPINFO")
            run_quiet(sh, "del /p PKGS/SIGTEST/SIGTEST.ASSETS")
            run_quiet(sh, "del /p SIGPUB.BIN")
            clear_key()
            out = run_quiet(sh, "pkg key show")
            check("key cleared", out, "no trusted key")
    finally:
        sh.close()

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
