using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using Windows.Security.Cryptography;
using Windows.Security.Cryptography.Core;
using Windows.Storage;

namespace Unigram.Logs
{
    public static class PushDiagnostics
    {
        private const long MaximumFileSize = 1024 * 1024;
        private static readonly object FileSyncRoot = new object();

        public const string DirectoryName = "Diagnostics";
        public const string FileName = "push-diagnostics.txt";

        public static void Write(string eventName, string details = null)
        {
            try
            {
                lock (FileSyncRoot)
                {
                    var filePath = GetFilePath();
                    if (string.IsNullOrEmpty(filePath))
                    {
                        return;
                    }

                    var directory = Path.GetDirectoryName(filePath);
                    Directory.CreateDirectory(directory);

                    if (File.Exists(filePath) && new FileInfo(filePath).Length >= MaximumFileSize)
                    {
                        File.Delete(filePath);
                    }

                    var line = $"{DateTimeOffset.UtcNow:O}|{eventName}";
                    if (!string.IsNullOrEmpty(details))
                    {
                        line += $"|{details}";
                    }

                    line += Environment.NewLine;
                    var bytes = Encoding.UTF8.GetBytes(line);
                    using (var stream = new FileStream(filePath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
                    {
                        stream.Write(bytes, 0, bytes.Length);
                    }
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Unable to write push diagnostics: 0x{ex.HResult:X8}");
            }
        }

        public static void WriteException(string eventName, Exception exception)
        {
            if (exception == null)
            {
                Write(eventName, "result=error;type=none");
                return;
            }

            string details;

            try
            {
#if MODERN_TDLIB
                // EventAggregator dispatches updates through MethodInfo.Invoke, so the
                // failure that matters is wrapped in a TargetInvocationException. Report the
                // innermost cause and keep the wrapper chain, which is type names only.
                var root = exception;
                var chain = root.GetType().Name;
                while (root is TargetInvocationException && root.InnerException != null)
                {
                    root = root.InnerException;
                    chain += ">" + root.GetType().Name;
                }

                details = $"result=error;hresult=0x{root.HResult:X8};type={root.GetType().Name};chain={chain}";

                // The parameter name carried by an argument exception identifies which value an
                // interop call rejected, which is what makes a marshalling failure actionable.
                // It is a compile-time identifier from the API surface, never user data.
                if (root is ArgumentException argument && !string.IsNullOrEmpty(argument.ParamName))
                {
                    details += $";param={argument.ParamName}";
                }

                details += $";message={SanitizeErrorMessage(root.Message)}";
#else
                details = $"result=error;hresult=0x{exception.HResult:X8};type={exception.GetType().Name}";
#endif
            }
            catch
            {
                // This runs inside a catch handler on TDLib's receive thread. Letting a
                // logging failure escape would reach the native caller and fail the process
                // fast, replacing the original fault with a less useful one.
                details = "result=error;type=unavailable";
            }

            Write(eventName, details);
        }

        public static string HashIdentifier(string value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return "none";
            }

            try
            {
                var provider = HashAlgorithmProvider.OpenAlgorithm(HashAlgorithmNames.Sha256);
                var input = CryptographicBuffer.ConvertStringToBinary(value, BinaryStringEncoding.Utf8);
                var hash = CryptographicBuffer.EncodeToHexString(provider.HashData(input));
                return hash.Substring(0, 12);
            }
            catch
            {
                return "hash_error";
            }
        }

        public static string SanitizeErrorMessage(string message)
        {
            if (string.IsNullOrWhiteSpace(message))
            {
                return "none";
            }

            try
            {
                var sanitized = message.Replace('\r', ' ').Replace('\n', ' ').Replace('|', '/');
                sanitized = Regex.Replace(sanitized, @"https?://\S+", "[redacted_uri]", RegexOptions.IgnoreCase);
                // Must run after the URI rule, otherwise it would consume "s://host" first.
                // Segments deliberately allow spaces so that a profile name such as
                // "C:\Users\John Smith\..." cannot survive redaction; over-redacting the
                // tail of a sentence is an acceptable trade for never emitting a path.
                sanitized = Regex.Replace(sanitized, @"[A-Za-z]:(?:[\\/][^\\/""\r\n]*)+", "[redacted_path]");
                sanitized = Regex.Replace(sanitized, @"\b\d{6,}\b", "[redacted_number]");
                sanitized = Regex.Replace(sanitized, @"\b[A-Za-z0-9_-]{24,}\b", "[redacted_token]");
                return sanitized.Length <= 256 ? sanitized : sanitized.Substring(0, 256);
            }
            catch
            {
                return "sanitize_error";
            }
        }

        private static string GetFilePath()
        {
            try
            {
                var localFolder = ApplicationData.Current?.LocalFolder;
                if (localFolder == null || string.IsNullOrEmpty(localFolder.Path))
                {
                    return null;
                }

                return Path.Combine(localFolder.Path, DirectoryName, FileName);
            }
            catch
            {
                return null;
            }
        }
    }
}
