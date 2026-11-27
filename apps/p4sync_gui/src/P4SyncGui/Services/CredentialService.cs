// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/credentials
// @layer app
// @domain P4Sync desktop client
// @provides OS credential store (Windows Credential Manager + DPAPI fallback)
// @status prototype
// @test README.md
// @hard-rule No secret is written to the repo, a log, or settings.json.
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace P4SyncGui.Services;

/// <summary>A credential read from the OS store. Never render Password in the UI.</summary>
public sealed record StoredCredential(string UserName, string Password);

/// <summary>
/// Windows Credential Manager first, DPAPI under %APPDATA% as a fallback.
/// The value can be handed to the bridge through environment variables at
/// process launch; it is never printed or persisted in the repository.
/// </summary>
public sealed class CredentialService
{
    private const uint CredTypeGeneric = 1;
    private const uint CredPersistLocalMachine = 3;
    private const uint CryptProtectUiForbidden = 0x1;

    private static string DpapiDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "P4SyncGui", "credentials");

    /// <summary>Save a secret; returns which store succeeded.</summary>
    public string Save(string target, string userName, string password)
    {
        if (SaveWindows(target, userName, password)) return "Windows Credential Manager";
        SaveDpapi(target, userName, password);
        return "DPAPI (user profile)";
    }

    public StoredCredential? Load(string target) => LoadWindows(target) ?? LoadDpapi(target);

    /// <summary>Only the user name is safe to display; the password is not.</summary>
    public string? DisplayUser(string target) => Load(target)?.UserName;

    public bool Has(string target) => Load(target) is not null;

    public bool Delete(string target)
    {
        bool removed = CredDeleteW(target, CredTypeGeneric, 0);
        string file = DpapiFile(target);
        if (File.Exists(file))
        {
            File.Delete(file);
            removed = true;
        }
        return removed;
    }

    // -- Windows Credential Manager --------------------------------------

    [StructLayout(LayoutKind.Sequential)]
    private struct FileTime
    {
        public uint Low;
        public uint High;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct CredentialStruct
    {
        public uint Flags;
        public uint Type;
        public IntPtr TargetName;
        public IntPtr Comment;
        public FileTime LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public IntPtr TargetAlias;
        public IntPtr UserName;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredWriteW(ref CredentialStruct credential, uint flags);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredReadW(string target, uint type, uint flags, out IntPtr credential);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredDeleteW(string target, uint type, uint flags);

    [DllImport("advapi32.dll")]
    private static extern void CredFree(IntPtr buffer);

    private static bool SaveWindows(string target, string userName, string password)
    {
        IntPtr targetPtr = Marshal.StringToHGlobalUni(target);
        IntPtr userPtr = Marshal.StringToHGlobalUni(userName);
        IntPtr blobPtr = Marshal.StringToHGlobalUni(password);
        try
        {
            var credential = new CredentialStruct
            {
                Flags = 0,
                Type = CredTypeGeneric,
                TargetName = targetPtr,
                Comment = IntPtr.Zero,
                CredentialBlobSize = (uint)(password.Length * 2),
                CredentialBlob = blobPtr,
                Persist = CredPersistLocalMachine,
                TargetAlias = IntPtr.Zero,
                UserName = userPtr,
            };
            return CredWriteW(ref credential, 0);
        }
        finally
        {
            Marshal.FreeHGlobal(targetPtr);
            Marshal.FreeHGlobal(userPtr);
            Marshal.FreeHGlobal(blobPtr);
        }
    }

    private static StoredCredential? LoadWindows(string target)
    {
        if (!CredReadW(target, CredTypeGeneric, 0, out IntPtr ptr) || ptr == IntPtr.Zero)
            return null;
        try
        {
            var credential = Marshal.PtrToStructure<CredentialStruct>(ptr);
            string user = credential.UserName == IntPtr.Zero
                ? "" : Marshal.PtrToStringUni(credential.UserName) ?? "";
            string password = credential.CredentialBlobSize == 0 || credential.CredentialBlob == IntPtr.Zero
                ? "" : Marshal.PtrToStringUni(credential.CredentialBlob,
                    (int)(credential.CredentialBlobSize / 2)) ?? "";
            return new StoredCredential(user, password);
        }
        finally
        {
            CredFree(ptr);
        }
    }

    // -- DPAPI fallback ---------------------------------------------------

    [StructLayout(LayoutKind.Sequential)]
    private struct DataBlob
    {
        public uint Size;
        public IntPtr Data;
    }

    [DllImport("crypt32.dll", SetLastError = true)]
    private static extern bool CryptProtectData(
        ref DataBlob input, string? description, ref DataBlob entropy,
        IntPtr reserved, IntPtr prompt, uint flags, out DataBlob output);

    [DllImport("crypt32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CryptUnprotectData(
        ref DataBlob input, out IntPtr description, ref DataBlob entropy,
        IntPtr reserved, IntPtr prompt, uint flags, out DataBlob output);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr handle);

    private static string DpapiFile(string target)
    {
        byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes(target));
        return Path.Combine(DpapiDirectory, Convert.ToHexString(hash)[..16] + ".dpapi");
    }

    private static void SaveDpapi(string target, string userName, string password)
    {
        byte[] plaintext = Encoding.UTF8.GetBytes(userName + "\n" + password);
        byte[] entropyBytes = Encoding.UTF8.GetBytes(target);
        IntPtr plainPtr = Marshal.AllocHGlobal(plaintext.Length);
        IntPtr entropyPtr = Marshal.AllocHGlobal(entropyBytes.Length);
        try
        {
            Marshal.Copy(plaintext, 0, plainPtr, plaintext.Length);
            Marshal.Copy(entropyBytes, 0, entropyPtr, entropyBytes.Length);
            var input = new DataBlob { Size = (uint)plaintext.Length, Data = plainPtr };
            var entropy = new DataBlob { Size = (uint)entropyBytes.Length, Data = entropyPtr };
            if (!CryptProtectData(ref input, "P4SyncGui", ref entropy, IntPtr.Zero,
                    IntPtr.Zero, CryptProtectUiForbidden, out var output))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            try
            {
                byte[] protectedBytes = new byte[output.Size];
                Marshal.Copy(output.Data, protectedBytes, 0, protectedBytes.Length);
                Directory.CreateDirectory(DpapiDirectory);
                File.WriteAllBytes(DpapiFile(target), protectedBytes);
            }
            finally
            {
                LocalFree(output.Data);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(plainPtr);
            Marshal.FreeHGlobal(entropyPtr);
        }
    }

    private static StoredCredential? LoadDpapi(string target)
    {
        string file = DpapiFile(target);
        if (!File.Exists(file)) return null;

        byte[] protectedBytes = File.ReadAllBytes(file);
        byte[] entropyBytes = Encoding.UTF8.GetBytes(target);
        IntPtr cipherPtr = Marshal.AllocHGlobal(protectedBytes.Length);
        IntPtr entropyPtr = Marshal.AllocHGlobal(entropyBytes.Length);
        try
        {
            Marshal.Copy(protectedBytes, 0, cipherPtr, protectedBytes.Length);
            Marshal.Copy(entropyBytes, 0, entropyPtr, entropyBytes.Length);
            var input = new DataBlob { Size = (uint)protectedBytes.Length, Data = cipherPtr };
            var entropy = new DataBlob { Size = (uint)entropyBytes.Length, Data = entropyPtr };
            if (!CryptUnprotectData(ref input, out IntPtr description, ref entropy, IntPtr.Zero,
                    IntPtr.Zero, CryptProtectUiForbidden, out var output))
                return null;
            try
            {
                byte[] plain = new byte[output.Size];
                Marshal.Copy(output.Data, plain, 0, plain.Length);
                string text = Encoding.UTF8.GetString(plain);
                int split = text.IndexOf('\n');
                return split < 0
                    ? new StoredCredential("", text)
                    : new StoredCredential(text[..split], text[(split + 1)..]);
            }
            finally
            {
                LocalFree(output.Data);
                if (description != IntPtr.Zero) LocalFree(description);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(cipherPtr);
            Marshal.FreeHGlobal(entropyPtr);
        }
    }
}
