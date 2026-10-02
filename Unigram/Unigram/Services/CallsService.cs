using libtgvoip;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Telegram.Td.Api;
using Unigram.Common;
using Unigram.Controls;
using Unigram.Services.Updates;
using Unigram.Services.ViewService;
using Unigram.ViewModels;
using Unigram.Views;
using Unigram.Views.Popups;
using Windows.Media.Core;
using Windows.Media.Playback;
using Windows.Storage;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Unigram.Services
{
    public interface IVoIPService : IHandle<UpdateCall>
    {
        string CurrentAudioInput { get; set; }
        float CurrentVolumeInput { get; set; }

        string CurrentAudioOutput { get; set; }
        float CurrentVolumeOutput { get; set; }

        Call ActiveCall { get; }

        void Show();
    }

    public class VoIPService : TLViewModelBase, IVoIPService
    {
        private readonly IViewService _viewService;

        private readonly MediaPlayer _mediaPlayer;
        private static int _audioCallDiagnosticBudget = 64;

        private Call _call;
        private DateTime _callStarted;
        private VoIPControllerWrapper _controller;

        private VoIPPage _callPage;
        private OverlayPage _callDialog;
        private ViewLifetimeControl _callLifetime;

        public VoIPService(IProtoService protoService, ICacheService cacheService, ISettingsService settingsService, IEventAggregator aggregator, IViewService viewService)
            : base(protoService, cacheService, settingsService, aggregator)
        {
            _viewService = viewService;

            if (ApiInfo.IsMediaSupported)
            {
                _mediaPlayer = new MediaPlayer();
                _mediaPlayer.CommandManager.IsEnabled = false;
                _mediaPlayer.AudioDeviceType = MediaPlayerAudioDeviceType.Communications;
                _mediaPlayer.AudioCategory = MediaPlayerAudioCategory.Communications;
            }

            aggregator.Subscribe(this);

            _watcher = Windows.Devices.Enumeration.DeviceInformation.CreateWatcher(Windows.Devices.Sensors.ProximitySensor.GetDeviceSelector());
            _watcher.Added += OnProximitySensorAdded;
            _watcher.Start();
        }

        #region Proximity

        private Windows.Devices.Sensors.ProximitySensor _sensor;
        private Windows.Devices.Sensors.ProximitySensorDisplayOnOffController _displayController;
        private Windows.Devices.Enumeration.DeviceWatcher _watcher;

        /// <summary>
        /// Invoked when the device watcher finds a proximity sensor
        /// </summary>
        /// <param name="sender">The device watcher</param>
        /// <param name="device">Device information for the proximity sensor that was found</param>
        private void OnProximitySensorAdded(Windows.Devices.Enumeration.DeviceWatcher sender, Windows.Devices.Enumeration.DeviceInformation device)
        {
            if (_sensor == null && Windows.Devices.Sensors.ProximitySensor.FromId(device.Id) is Windows.Devices.Sensors.ProximitySensor foundSensor)
                _sensor = foundSensor;
        }

        private void EnableDisplayOnOffController()
        {
            if (_sensor != null && _displayController == null)
            {
                // Acquires the display on/off controller for this proximity sensor.
                // This tells the system to use the sensor's IsDetected state to
                // turn the screen on or off.  If the display does not support this
                // feature, this code will do nothing.
                _displayController = _sensor.CreateDisplayOnOffController();
            }
        }

        private void DisableDisplayOnOffController()
        {
            if (_displayController != null)
            {
                _displayController.Dispose(); // closes the controller
                _displayController = null;
            }
        }

        #endregion

        public string CurrentAudioInput
        {
            get
            {
                return _controller?.CurrentAudioInput ?? SettingsService.Current.VoIP.InputDevice;
            }
            set
            {
                SettingsService.Current.VoIP.InputDevice = value;

                if (_controller != null)
                {
                    _controller.CurrentAudioInput = value;
                }
            }
        }

        public float CurrentVolumeInput
        {
            get
            {
                return SettingsService.Current.VoIP.InputVolume;
            }
            set
            {
                SettingsService.Current.VoIP.InputVolume = value;

                if (_controller != null)
                {
                    _controller.SetInputVolume(value);
                }
            }
        }

        public string CurrentAudioOutput
        {
            get
            {
                return _controller?.CurrentAudioOutput ?? SettingsService.Current.VoIP.OutputDevice;
            }
            set
            {
                SettingsService.Current.VoIP.OutputDevice = value;

                if (_controller != null)
                {
                    _controller.CurrentAudioOutput = value;
                }
            }
        }

        public float CurrentVolumeOutput
        {
            get
            {
                return SettingsService.Current.VoIP.OutputVolume;
            }
            set
            {
                SettingsService.Current.VoIP.OutputVolume = value;

                if (_controller != null)
                {
                    _controller.SetOutputVolume(value);
                }
            }
        }

        public async void Handle(UpdateCall update)
        {
            if (update?.Call == null)
            {
                WriteAudioCallDiagnostic("voip.update", "result=ignored;reason=call_unavailable");
                return;
            }

            _call = update.Call;
            WriteAudioCallDiagnostic("voip.update", $"result=received;state={update.Call.State?.GetType().Name ?? "null"};outgoing={update.Call.IsOutgoing.ToString().ToLowerInvariant()};video={update.Call.IsVideo.ToString().ToLowerInvariant()}");

            if (update.Call.IsVideo)
            {
                WriteAudioCallDiagnostic("voip.update", "result=ignored;reason=video_unsupported");
                ProtoService.Send(ModernTdlibCompatibility.CreateDiscardCall(update.Call.Id, true, 0, 0));
                return;
            }

            if (update.Call.State is CallStatePending pending)
            {
                if (update.Call.IsOutgoing && pending.IsCreated && pending.IsReceived)
                {
                    if (pending.IsCreated && pending.IsReceived)
                    {
                        PlayTone("voip_ringback.mp3", true);
                    }
                }
            }
            if (update.Call.State is CallStateReady ready)
            {
                if (ready.Protocol == null || ready.EncryptionKey == null)
                {
                    WriteAudioCallDiagnostic("voip.ready", "result=skipped;reason=invalid_parameters");
                    ProtoService.Send(ModernTdlibCompatibility.CreateDiscardCall(update.Call.Id, true, 0, 0));
                    return;
                }

                var user = CacheService.GetUser(update.Call.UserId);
                if (user == null)
                {
                    WriteAudioCallDiagnostic("voip.ready", "result=skipped;reason=user_unavailable");
                    return;
                }

                VoIPControllerWrapper.UpdateServerConfig(ready.Config);

                var logFile = Path.Combine(ApplicationData.Current.LocalFolder.Path, $"{SessionId}", $"voip{update.Call.Id}.txt");
                var statsDumpFile = Path.Combine(ApplicationData.Current.LocalFolder.Path, $"{SessionId}", "tgvoip.statsDump.txt");

                var call_packet_timeout_ms = CacheService.Options.CallPacketTimeoutMs;
                var call_connect_timeout_ms = CacheService.Options.CallConnectTimeoutMs;

                if (_controller != null)
                {
                    _controller.Dispose();
                    _controller = null;
                }

                var config = new VoIPConfig
                {
                    initTimeout = call_packet_timeout_ms / 1000.0,
                    recvTimeout = call_connect_timeout_ms / 1000.0,
                    dataSaving = GetVoipDataSavingMode(base.Settings.UseLessData),
                    enableAEC = true,
                    enableNS = true,
                    enableAGC = true,

                    enableVolumeControl = true,

                    logFilePath = logFile,
                    statsDumpFilePath = statsDumpFile
                };
                
                _controller = new VoIPControllerWrapper();
                _controller.SetConfig(config);
                _controller.CurrentAudioInput = SettingsService.Current.VoIP.InputDevice;
                _controller.CurrentAudioOutput = SettingsService.Current.VoIP.OutputDevice;
                _controller.SetInputVolume(SettingsService.Current.VoIP.InputVolume);
                _controller.SetOutputVolume(SettingsService.Current.VoIP.OutputVolume);

                _controller.CallStateChanged += (s, args) =>
                {
                    var error = args == libtgvoip.CallState.Failed
                        ? $";error={s.GetLastError()}"
                        : string.Empty;
                    WriteAudioCallDiagnostic("voip.transport", $"result=state;state={args}{error}");

                    BeginOnUIThread(() =>
                    {
                        if (args == libtgvoip.CallState.WaitInit || args == libtgvoip.CallState.WaitInitAck)
                        {
                            PlayTone("voip_connecting.mp3", false);
                        }
                        else if (args == libtgvoip.CallState.Established)
                        {
                            _callStarted = DateTime.Now;
                            StopTone();
                        }
                    });
                };

                BeginOnUIThread(() =>
                {
                    Show(update.Call, _controller, _callStarted);
                });

                var endpoints = new List<Endpoint>();
                var webRtcEndpointCount = 0;

                foreach (var server in ready.Servers ?? new CallServer[0])
                {
                    if (server.Type is CallServerTypeTelegramReflector telegramReflector)
                    {
                        endpoints.Add(new Endpoint
                        {
                            id = server.Id,
                            ipv4 = server.IpAddress,
                            ipv6 = server.Ipv6Address,
                            peerTag = telegramReflector.PeerTag.ToArray(),
                            port = (ushort)server.Port
                        });
                    }
                    else if (server.Type is CallServerTypeWebrtc)
                    {
                        webRtcEndpointCount++;
                    }
                }

                if (endpoints.Count == 0)
                {
                    WriteAudioCallDiagnostic("voip.ready", $"result=skipped;reason=no_reflector_endpoint;webrtc_endpoints={webRtcEndpointCount}");
                    _controller.Dispose();
                    _controller = null;
                    ProtoService.Send(ModernTdlibCompatibility.CreateDiscardCall(update.Call.Id, true, 0, 0));
                    return;
                }

                WriteAudioCallDiagnostic("voip.ready", $"result=starting;reflector_endpoints={endpoints.Count};webrtc_endpoints={webRtcEndpointCount};p2p={ready.Protocol.UdpP2p && ready.AllowP2p};protocol_min={ready.Protocol.MinLayer};protocol_max={ready.Protocol.MaxLayer};library_versions={ready.Protocol.LibraryVersions?.Count ?? 0}");
                _controller.SetEncryptionKey(ready.EncryptionKey.ToArray(), update.Call.IsOutgoing);
                _controller.SetPublicEndpoints(endpoints.ToArray(), ready.Protocol.UdpP2p && ready.AllowP2p, ready.Protocol.MaxLayer);
                _controller.Start();
                _controller.Connect();
            }
            else if (update.Call.State is CallStateDiscarded discarded)
            {
                if (discarded.NeedDebugInformation && _controller != null)
                {
                    ProtoService.Send(ModernTdlibCompatibility.CreateSendCallDebugInformation(update.Call.Id, _controller.GetDebugLog()));
                }

                if (discarded.NeedRating)
                {
                    BeginOnUIThread(async () => await SendRatingAsync(update.Call.Id));
                }

                _controller?.Dispose();
                _controller = null;
                _call = null;
            }

            await Dispatcher.DispatchAsync(() =>
            {
                switch (update.Call.State)
                {
                    case CallStateDiscarded discarded:
                        if (update.Call.IsOutgoing && discarded.Reason is CallDiscardReasonDeclined)
                        {
                            PlayTone("voip_busy.mp3", true);

                            Show(update.Call, null, _callStarted);
                        }
                        else
                        {
                            StopTone();

                            Hide();
                        }
                        break;
                    case CallStateError error:
                        WriteAudioCallDiagnostic("voip.error", $"code={error.Error?.Code ?? 0}");
                        StopTone();
                        Hide();
                        break;
                    default:
                        Show(update.Call, null, _callStarted);
                        break;
                }
            });
        }

        private static bool CanWriteAudioCallDiagnostic()
        {
            return System.Threading.Interlocked.Decrement(ref _audioCallDiagnosticBudget) >= 0;
        }

        private static void WriteAudioCallDiagnostic(string eventName, string details)
        {
            if (CanWriteAudioCallDiagnostic())
            {
                Logs.PushDiagnostics.Write(eventName, details);
            }
        }

        private void PlayTone(string fileName, bool isLooping)
        {
            if (_mediaPlayer == null)
            {
                WriteAudioCallDiagnostic("voip.tone", "result=skipped;reason=media_unavailable");
                return;
            }

            _mediaPlayer.Source = MediaSource.CreateFromUri(new Uri("ms-appx:///Assets/Audio/" + fileName));
            _mediaPlayer.IsLoopingEnabled = isLooping;
            _mediaPlayer.Play();
        }

        private void StopTone()
        {
            if (_mediaPlayer != null)
            {
                _mediaPlayer.Source = null;
            }
        }

        private async Task SendRatingAsync(int callId)
        {
            var dialog = new CallRatingPopup();

            var confirm = await dialog.ShowQueuedAsync();
            if (confirm == ContentDialogResult.Primary)
            {
                // We need updates here
                //await ProtoService.SendAsync(new SendCallRating(callId, dialog.Rating, dialog.Rating >= 1 && dialog.Rating <= 4 ? dialog.Comment : string.Empty));

                if (dialog.IncludeDebugLogs && dialog.Rating <= 3)
                {
                    var file = await ApplicationData.Current.LocalFolder.TryGetItemAsync(Path.Combine($"{SessionId}", $"voip{callId}.txt")) as StorageFile;
                    if (file == null)
                    {
                        return;
                    }

                    var chat = await ProtoService.SendAsync(new CreatePrivateChat(4244000, false)) as Chat;
                    if (chat == null)
                    {
                        return;
                    }

                    ProtoService.Send(ModernTdlibCompatibility.CreateSendMessage(
                        chat.Id,
                        0,
                        ModernTdlibCompatibility.CreateMessageSendOptions(false, false, null),
                        ModernTdlibCompatibility.CreateInputMessageDocument(new InputFileLocal(file.Path), null, false, null)));
                }
            }
        }

        private static libtgvoip.DataSavingMode GetVoipDataSavingMode(DataSavingMode value)
        {
            switch (value)
            {
                case DataSavingMode.MobileOnly:
                    return libtgvoip.DataSavingMode.MobileOnly;
                case DataSavingMode.Always:
                    return libtgvoip.DataSavingMode.Always;
                default:
                    return libtgvoip.DataSavingMode.Never;
            }
        }

        public Call ActiveCall
        {
            get
            {
                return _call;
            }
        }

        public async void Show()
        {
            if (_call == null)
            {
                return;
            }

            Show(_call, _controller, _callStarted);

            if (_callDialog != null)
            {
                _callDialog.IsOpen = true;
            }
            else if (_callLifetime != null)
            {
                _callLifetime = await _viewService.OpenAsync(() => _callPage = _callPage ?? new VoIPPage(ProtoService, CacheService, Aggregator, _call, _controller, _callStarted), _call.Id);
                _callLifetime.WindowWrapper.ApplicationView().Consolidated -= ApplicationView_Consolidated;
                _callLifetime.WindowWrapper.ApplicationView().Consolidated += ApplicationView_Consolidated;
            }

            Aggregator.Publish(new UpdateCallDialog(_call, true));
        }

        private async void Show(Call call, VoIPControllerWrapper controller, DateTime started)
        {
            if (_callPage == null)
            {
                if (ApplicationView.GetForCurrentView().IsViewModeSupported(ApplicationViewMode.CompactOverlay))
                {
                    _callLifetime = await _viewService.OpenAsync(() => _callPage = _callPage ?? new VoIPPage(ProtoService, CacheService, Aggregator, _call, _controller, _callStarted), call.Id);
                    _callLifetime.WindowWrapper.ApplicationView().Consolidated -= ApplicationView_Consolidated;
                    _callLifetime.WindowWrapper.ApplicationView().Consolidated += ApplicationView_Consolidated;
                }
                else
                {
                    _callPage = new VoIPPage(ProtoService, CacheService, Aggregator, _call, _controller, _callStarted);

                    _callDialog = new OverlayPage();
                    _callDialog.HorizontalAlignment = HorizontalAlignment.Stretch;
                    _callDialog.VerticalAlignment = VerticalAlignment.Stretch;
                    _callDialog.Content = _callPage;
                    _callDialog.IsOpen = true;
                }

                Aggregator.Publish(new UpdateCallDialog(call, true));
            }

            await _callPage.Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
            {
                if (controller != null)
                {
                    _callPage.Connect(controller);
                }

                _callPage.Update(call, started);
            });
            EnableDisplayOnOffController();
        }

        private async void Hide()
        {
            if (_callPage != null)
            {
                await _callPage.Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
                {
                    if (_callDialog != null)
                    {
                        _callDialog.IsOpen = false;
                        _callDialog = null;
                    }
                    else if (_callLifetime != null)
                    {
                        _callLifetime.StopViewInUse();
                        _callLifetime.WindowWrapper.Window.Close();
                        _callLifetime = null;
                    }

                    _callPage.Dispose();
                    _callPage = null;
                });

                Aggregator.Publish(new UpdateCallDialog(_call, true));
            }
            DisableDisplayOnOffController();
        }

        private void ApplicationView_Consolidated(ApplicationView sender, ApplicationViewConsolidatedEventArgs args)
        {
            if (_callLifetime != null)
            {
                _callLifetime.StopViewInUse();
                _callLifetime.WindowWrapper.Window.Close();
                _callLifetime = null;
            }

            if (_callPage != null)
            {
                _callPage.Dispose();
                _callPage = null;
            }

            Aggregator.Publish(new UpdateCallDialog(_call, false));
        }
    }
}
