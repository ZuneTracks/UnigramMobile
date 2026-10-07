using System;
using Telegram.Td.Api;
using Unigram.Converters;
using Unigram.Services;
using Unigram.ViewModels;
using Unigram.Views.Popups;
using Windows.Storage;
using Windows.UI.Xaml;

namespace Unigram.Views
{
    public sealed partial class DiagnosticsPage : HostedPage
    {
        public DiagnosticsViewModel ViewModel => DataContext as DiagnosticsViewModel;

        public DiagnosticsPage()
        {
            InitializeComponent();
            DataContext = TLContainer.Current.Resolve<DiagnosticsViewModel>();
        }

        #region Binding

        private string ConvertVerbosity(VerbosityLevel level)
        {
            return Enum.GetName(typeof(VerbosityLevel), level);
        }

        private string ConvertSize(ulong size)
        {
            return FileSizeConverter.Convert((long)size);
        }

        #endregion

        private async void Log_Click(object sender, RoutedEventArgs e)
        {
            var log = await ApplicationData.Current.LocalFolder.TryGetItemAsync("tdlib_log.txt") as StorageFile;
            if (log != null)
            {
                await SharePopup.GetForCurrentView().ShowAsync(ModernTdlibCompatibility.CreateInputMessageDocument(new InputFileLocal(log.Path), null, true, null));
            }
        }

        private async void LogOld_Click(object sender, RoutedEventArgs e)
        {
            var log = await ApplicationData.Current.LocalFolder.TryGetItemAsync("tdlib_log.txt.old") as StorageFile;
            if (log != null)
            {
                await SharePopup.GetForCurrentView().ShowAsync(ModernTdlibCompatibility.CreateInputMessageDocument(new InputFileLocal(log.Path), null, true, null));
            }
        }

        private async void PushLog_Click(object sender, RoutedEventArgs e)
        {
            // Native fault records are appended to a separate file, so fold them in
            // before sharing rather than leaving them for the next call to collect.
            Logs.PushDiagnostics.DrainFaultFile();

            var folder = await ApplicationData.Current.LocalFolder.TryGetItemAsync(Logs.PushDiagnostics.DirectoryName) as StorageFolder;
            var log = folder == null ? null : await folder.TryGetItemAsync(Logs.PushDiagnostics.FileName) as StorageFile;
            if (log != null)
            {
                await SharePopup.GetForCurrentView().ShowAsync(ModernTdlibCompatibility.CreateInputMessageDocument(new InputFileLocal(log.Path), null, true, null));
            }
        }
    }
}
