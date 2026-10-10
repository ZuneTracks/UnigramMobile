using System;
using System.Linq;
using System.Threading.Tasks;
using Telegram.Td.Api;
using Unigram.Collections;
using Unigram.Common;
using Unigram.Controls;
using Unigram.Logs;
using Unigram.Services;
using Unigram.Views.Popups;
using Windows.Storage;
using Windows.UI.Xaml.Media.Imaging;

namespace Unigram.ViewModels.BasicGroups
{
    public class BasicGroupCreateStep1ViewModel : TLViewModelBase
    {
        public BasicGroupCreateStep1ViewModel(IProtoService protoService, ICacheService cacheService, ISettingsService settingsService, IEventAggregator aggregator)
            : base(protoService, cacheService, settingsService, aggregator)
        {
            Items = new MvxObservableCollection<Chat>();

            AddCommand = new RelayCommand(AddExecute);
            SendCommand = new RelayCommand(SendExecute, CanSend);
            EditPhotoCommand = new RelayCommand<StorageFile>(EditPhotoExecute);
        }

        private bool _isCreating;

        private string _title;
        public string Title
        {
            get
            {
                return _title;
            }
            set
            {
                Set(ref _title, value);
                SendCommand.RaiseCanExecuteChanged();
            }
        }

        private BitmapImage _preview;
        public BitmapImage Preview
        {
            get
            {
                return _preview;
            }
            set
            {
                Set(ref _preview, value);
            }
        }

        public MvxObservableCollection<Chat> Items { get; private set; }

        public RelayCommand AddCommand { get; }
        private async void AddExecute()
        {
            var chats = await SharePopup.PickChatsAsync(Strings.Resources.SelectContacts, Items.Select(x => x.Id).ToArray());
            if (chats != null)
            {
                Items.ReplaceWith(chats);
            }

            SendCommand.RaiseCanExecuteChanged();
        }

        public RelayCommand SendCommand { get; }
        private bool CanSend()
        {
            return !_isCreating && !string.IsNullOrWhiteSpace(Title) && Items.Count > 0;
        }

        private async void SendExecute()
        {
            if (_isCreating)
            {
                return;
            }

            _isCreating = true;
            SendCommand.RaiseCanExecuteChanged();

            var maxSize = CacheService.Options.BasicGroupSizeMax;

            try
            {
                var peers = Items.Select(x => x.Type).OfType<ChatTypePrivate>().Select(x => x.UserId).ToArray();
                if (peers.Length <= maxSize)
                {
                    var response = await ProtoService.SendAsync(ModernTdlibCompatibility.CreateNewBasicGroupChat(peers, _title));
#if MODERN_TDLIB
                    if (response is CreatedBasicGroupChat created)
                    {
                        var chat = await ProtoService.SendAsync(new GetChat(created.ChatId)) as Chat;
                        if (chat != null)
                        {
                            await CompleteCreationAsync(chat);
                        }
                        else
                        {
                            PushDiagnostics.Write("group.create", "result=created_chat_unavailable");
                            await MessagePopup.ShowAsync(Strings.Resources.ErrorOccurred, Strings.Resources.AppName, Strings.Resources.OK);
                        }
                    }
#else
                    if (response is Chat chat)
                    {
                        await CompleteCreationAsync(chat);
                    }
#endif
                    else if (response is Error error)
                    {
                        AlertsService.ShowAddUserAlert(Dispatcher, error.Message, false);
                    }
                    else
                    {
                        PushDiagnostics.Write("group.create", $"result=unexpected_response;type={response?.GetType().Name ?? "null"}");
                        await MessagePopup.ShowAsync(Strings.Resources.ErrorOccurred, Strings.Resources.AppName, Strings.Resources.OK);
                    }
                }
                else
                {
                    await MessagePopup.ShowAsync(Strings.Resources.ErrorOccurred, Strings.Resources.AppName, Strings.Resources.OK);
                }
            }
            finally
            {
                _isCreating = false;
                SendCommand.RaiseCanExecuteChanged();
            }
        }

        private async Task CompleteCreationAsync(Chat chat)
        {
            await MessagePopup.ShowAsync(
                Strings.Resources.ActionYouCreateGroup,
                chat.Title,
                Strings.Resources.OpenGroup);

            NavigationService.NavigateToChat(chat);
            NavigationService.GoBackAt(0, false);
        }

        public RelayCommand<StorageFile> EditPhotoCommand { get; }
        private async void EditPhotoExecute(StorageFile file)
        {
            await Task.CompletedTask;
        }

        private void ContinueUploadingPhoto()
        {
            //NavigationService.Navigate(typeof(BasicGroupCreateStep2Page), new ChatCreateStep2Tuple(_title, null));
        }
    }
}
