// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace System.Security.Cryptography.X509Certificates
{
    internal sealed partial class StorePal
    {
        private sealed class TrustedStore : IStorePal
        {
            private readonly StoreLocation _location;

            internal TrustedStore(StoreLocation location)
            {
                _location = location;
            }

            public SafeHandle? SafeHandle => null;

            public void Dispose()
            {
            }

            public void Add(ICertificatePal cert)
            {
                throw new CryptographicException(SR.Cryptography_X509_StoreReadOnly);
            }

            public void Remove(ICertificatePal cert)
            {
                throw new CryptographicException(SR.Cryptography_X509_StoreReadOnly);
            }

            public unsafe void CloneTo(X509Certificate2Collection collection)
            {
                EnumCertificatesContext context = default;
                context.Results = new HashSet<X509Certificate2>();

                bool systemOnly = _location == StoreLocation.LocalMachine;
                unsafe
                {
                    bool success = Interop.AndroidCrypto.X509StoreEnumerateTrustedCertificates(
                        (byte)(systemOnly ? 1 : 0),
                        &EnumCertificatesCallback,
                        &context);
                    if (!success)
                    {
                        throw new CryptographicException(SR.Cryptography_X509_StoreEnumerateFailure);
                    }
                }

                foreach (X509Certificate2 cert in context.Results)
                {
                    collection.Add(cert);
                }
            }

            private struct EnumCertificatesContext
            {
                public HashSet<X509Certificate2> Results;
            }

            [UnmanagedCallersOnly]
            private static unsafe void EnumCertificatesCallback(void* certPtr, void* context)
            {
                EnumCertificatesContext* callbackContext = (EnumCertificatesContext*)context;
                var handle = new SafeX509Handle((IntPtr)certPtr);
                var cert = new X509Certificate2(new AndroidCertificatePal(handle));
                if (!callbackContext->Results.Add(cert))
                    cert.Dispose();
            }
        }
    }
}
