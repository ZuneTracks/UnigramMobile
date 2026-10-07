using libtgvoip;
using Microsoft.Graphics.Canvas.Effects;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Numerics;
using Telegram.Td.Api;
using Unigram.Common;
using Unigram.Controls;
using Unigram.Services;
using Unigram.Views.Popups;
using Windows.Foundation;
using Windows.Foundation.Metadata;
using Windows.Phone.Media.Devices;
using Windows.UI;
using Windows.UI.Composition;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Hosting;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using Windows.UI.Xaml.Shapes;
using Point = Windows.Foundation.Point;
#if MODERN_TGCALLS
using ModernCalls = Unigram.Native.Calls.Proof;
#endif

namespace Unigram.Views
{
    public sealed partial class VoIPPage : Page, IDisposable
    {
        private Visual _descriptionVisual;
        private Visual _largeVisual;
        private SpriteVisual _blurVisual;
        private CompositionEffectBrush _blurBrush;
        private Compositor _compositor;

        private bool _collapsed = true;

        private readonly IProtoService _protoService;
        private readonly ICacheService _cacheService;
        private readonly IEventAggregator _aggregator;

        private VoIPControllerWrapper _controller;
        private Call _call;

        private libtgvoip.CallState _state;

        // The modern tgcalls bridge replaces VoIPControllerWrapper, so Connect() is never
        // called and OnCallStateChanged never fires. This tracks the bridge's transport
        // state instead, so the duration timer and the state label behave the same way.
        private bool _modernEstablished;
        private IList<string> _emojis;
        private DateTime _started;

        private int _debugTapped;
        private ContentDialog _debugDialog;

        private DispatcherTimer _debugTimer;
        private DispatcherTimer _durationTimer;
        private AudioRoutingManager _audioRoutingManager;

        private bool _disposed;

        public OverlayPage Dialog { get; set; }

        public VoIPPage(IProtoService protoService, ICacheService cacheService, IEventAggregator aggregator, Call call, VoIPControllerWrapper controller, DateTime started)
        {
            Logs.PushDiagnostics.Write("voip.ui", "stage=construct_begin");
            this.InitializeComponent();
            Logs.PushDiagnostics.Write("voip.ui", "stage=initialized");

            _protoService = protoService;
            _cacheService = cacheService;
            _aggregator = aggregator;

            _durationTimer = new DispatcherTimer();
            _durationTimer.Interval = TimeSpan.FromMilliseconds(500);
            _durationTimer.Tick += DurationTimer_Tick;

            _debugTimer = new DispatcherTimer();
            _debugTimer.Interval = TimeSpan.FromMilliseconds(500);
            _debugTimer.Tick += DebugTimer_Tick;

            #region Reset

            LargeEmoji0.Source = null;
            LargeEmoji1.Source = null;
            LargeEmoji2.Source = null;
            LargeEmoji3.Source = null;

            #endregion

            #region Composition

            _descriptionVisual = ElementCompositionPreview.GetElementVisual(DescriptionLabel);
            _largeVisual = ElementCompositionPreview.GetElementVisual(LargePanel);
            _compositor = _largeVisual.Compositor;

            var graphicsEffect = new GaussianBlurEffect
            {
                Name = "Blur",
                BlurAmount = 0,
                BorderMode = EffectBorderMode.Hard,
                Source = new CompositionEffectSourceParameter("backdrop")
            };

            var effectFactory = _compositor.CreateEffectFactory(graphicsEffect, new[] { "Blur.BlurAmount" });
            var effectBrush = effectFactory.CreateBrush();
            var backdrop = _compositor.CreateBackdropBrush();
            effectBrush.SetSourceParameter("backdrop", backdrop);

            _blurBrush = effectBrush;
            _blurVisual = _compositor.CreateSpriteVisual();
            _blurVisual.Brush = _blurBrush;

            // Why does this crashes due to an access violation exception on certain devices?
            ElementCompositionPreview.SetElementChildVisual(BlurPanel, _blurVisual);
            Logs.PushDiagnostics.Write("voip.ui", "stage=composition_ready");

            #endregion

            var titleBar = ApplicationView.GetForCurrentView().TitleBar;
            if (titleBar != null)
            {
                titleBar.ButtonBackgroundColor = Colors.Transparent;
                titleBar.ButtonForegroundColor = Colors.White;
                titleBar.ButtonInactiveBackgroundColor = Colors.Transparent;
                titleBar.ButtonInactiveForegroundColor = Colors.White;
                Window.Current.SetTitleBar(BlurPanel);
            }
            else
            {
                Logs.PushDiagnostics.Write("voip.ui", "result=title_bar_unavailable");
            }

            Logs.PushDiagnostics.Write("voip.ui", "stage=chrome_ready");

            if (call != null)
            {
                Update(call, started);
                Logs.PushDiagnostics.Write("voip.ui", "stage=call_updated");
            }

            if (controller != null)
            {
                Connect(controller);
                Logs.PushDiagnostics.Write("voip.ui", "stage=controller_connected");
            }
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            if (Routing == null)
            {
                Logs.PushDiagnostics.Write("voip.ui", "result=routing_skipped;reason=control_unavailable");
                return;
            }

            if (!ApiInfo.IsPhoneContractPresent)
            {
                Routing.Visibility = Visibility.Collapsed;
                Logs.PushDiagnostics.Write("voip.ui", "result=routing_skipped;reason=no_phone_contract");
                return;
            }

            _audioRoutingManager = AudioRoutingManager.GetDefault();
            if (_audioRoutingManager == null)
            {
                Routing.Visibility = Visibility.Collapsed;
                Logs.PushDiagnostics.Write("voip.ui", "result=routing_unavailable");
                return;
            }

            Routing.Visibility = Visibility.Visible;
            _audioRoutingManager.AudioEndpointChanged += AudioEndpointChanged;
            Routing.IsChecked = _audioRoutingManager.GetAudioEndpoint() == AudioRoutingEndpoint.Speakerphone;
            Logs.PushDiagnostics.Write(
                "voip.ui",
                $"result=routing_ready;endpoint={_audioRoutingManager.GetAudioEndpoint()};available={_audioRoutingManager.AvailableAudioEndpoints}");
        }

        private void OnUnloaded(object sender, RoutedEventArgs e)
        {
            Debug.WriteLine("Unloaded");

            if (_audioRoutingManager != null)
            {
                _audioRoutingManager.AudioEndpointChanged -= AudioEndpointChanged;
            }
        }

        public void Dispose()
        {
            _disposed = true;
            _debugTimer.Stop();
            _durationTimer.Stop();

            if (_controller != null)
            {
                //_controller.CallStateChanged -= OnCallStateChanged;
                //_controller.SignalBarsChanged -= OnSignalBarsChanged;
                _controller = null;
            }

            if (_audioRoutingManager != null)
            {
                _audioRoutingManager.AudioEndpointChanged -= AudioEndpointChanged;
                _audioRoutingManager = null;
            }
        }

        public void Connect(VoIPControllerWrapper controller)
        {
            _controller = controller;

            // Let's avoid duplicated events
            _controller.CallStateChanged -= OnCallStateChanged;
            _controller.CallStateChanged += OnCallStateChanged;

            _controller.SignalBarsChanged -= OnSignalBarsChanged;
            _controller.SignalBarsChanged += OnSignalBarsChanged;

            _controller.SetMicMute(_isMuted);

            OnCallStateChanged(controller, controller.GetConnectionState());
            OnSignalBarsChanged(controller, controller.GetSignalBarsCount());
        }

        //private void CoreBar_IsVisibleChanged(CoreApplicationViewTitleBar sender, object args)
        //{
        //    Debug.WriteLine("TitleBar height: " + sender.Height);

        //    SmallEmojiLabel.Margin = new Thickness(sender.SystemOverlayLeftInset, 20, sender.SystemOverlayRightInset, 0);
        //    OnSizeChanged(null, null);
        //}

        private void OnSizeChanged(object sender, SizeChangedEventArgs e)
        {
            // The page raises its first SizeChanged as soon as it is measured, which can be
            // before the composition visuals have been created and again after Dispose has
            // torn them down. Both are ordinary, and neither justifies faulting the UI
            // thread over work that is purely decorative.
            if (_disposed || _blurVisual == null || _blurBrush == null)
            {
                return;
            }

            _blurVisual.Size = e.NewSize.ToVector2();

            if (_collapsed && SmallPanel != null && LargeEmojiLabel != null)
            {
                var transform = SmallPanel.TransformToVisual(LargeEmojiLabel);
                var position = transform.TransformPoint(new Point());

                _descriptionVisual.Opacity = 0;
                _largeVisual.Offset = new Vector3(position.ToVector2(), 0);
                _largeVisual.Scale = new Vector3(0.5f);
                _blurBrush.Properties.InsertScalar("Blur.BlurAmount", 0);
            }
        }

        public void Update(Call call, DateTime started)
        {
            if (_disposed)
            {
                return;
            }

            _call = call;
            _started = started;

            //if (_state != call.State)
            //{
            //    Debug.WriteLine("[{0:HH:mm:ss.fff}] State changed in app: " + tuple.Item1, DateTime.Now);

            //    _state = tuple.Item1;
            //    StateLabel.Content = StateToLabel(tuple.Item1);

            //    if (tuple.Item1 == TLPhoneCallState.Established)
            //    {
            //        SignalBarsLabel.Visibility = Visibility.Visible;
            //        StartUpdatingCallDuration();

            //        if (_emojis != null)
            //        {
            //            for (int i = 0; i < _emojis.Length; i++)
            //            {
            //                var imageLarge = FindName($"LargeEmoji{i}") as Image;
            //                var source = Emoji.BuildUri(_emojis[i]);

            //                imageLarge.Source = new BitmapImage(new Uri(source));
            //            }
            //        }
            //    }
            //}

            //if (tuple.Item2 is TLPhoneCallRequested call)
            //{
            //}

            var user = _cacheService.GetUser(call.UserId);
            if (user != null)
            {
                if (user.ProfilePhoto != null)
                {
                    var file = user.ProfilePhoto.Big;
                    if (file?.Local?.IsDownloadingCompleted == true)
                    {
                        Image.Source = new BitmapImage(new Uri("file:///" + file.Local.Path));
                        BackgroundPanel.Background = new SolidColorBrush(Colors.Transparent);
                    }
                    else if (file?.Local != null && file.Local.CanBeDownloaded && !file.Local.IsDownloadingActive)
                    {
                        Image.Source = null;
                        BackgroundPanel.Background = PlaceholderHelper.GetBrush(user.Id);

                        _protoService?.DownloadFile(file.Id, 1, 0);
                    }
                    else
                    {
                        Image.Source = null;
                        BackgroundPanel.Background = PlaceholderHelper.GetBrush(user.Id);
                    }
                }
                else
                {
                    Image.Source = null;
                    BackgroundPanel.Background = PlaceholderHelper.GetBrush(user.Id);
                }

                FromLabel.Text = user.GetFullName();
                DescriptionLabel.Text = string.Format(Strings.Resources.CallEmojiKeyTooltip, user.FirstName);
            }

            if (call.State is CallStateReady ready)
            {
                _emojis = ready.Emojis;

                for (int i = 0; i < ready.Emojis.Count; i++)
                {
                    var imageLarge = FindName($"LargeEmoji{i}") as Image;
                    var source = Emoji.BuildUri(_emojis[i]);

                    imageLarge.Source = new BitmapImage(new Uri(source));
                }
            }

            switch (call.State)
            {
                case CallStatePending pending:
                    if (call.IsOutgoing)
                    {
                        ResetUI();
                    }
                    else
                    {
                        Mute.Visibility = Visibility.Collapsed;

                        Close.Visibility = Visibility.Collapsed;
                        Close.Margin = new Thickness();

                        Accept.Margin = new Thickness(0, 0, 6, 0);
                        Accept.Visibility = Visibility.Visible;

                        Discard.Margin = new Thickness(6, 0, 0, 0);
                        Discard.Visibility = Visibility.Visible;
                    }
                    break;
                case CallStateDiscarded discarded:
                    if (call.IsOutgoing && discarded.Reason is CallDiscardReasonDeclined)
                    {
                        Mute.Visibility = Visibility.Collapsed;

                        Close.Margin = new Thickness(0, 0, 6, 0);
                        Close.Visibility = Visibility.Visible;

                        Accept.Margin = new Thickness(6, 0, 0, 0);
                        Accept.Visibility = Visibility.Visible;

                        Discard.Visibility = Visibility.Collapsed;
                        Discard.Margin = new Thickness();
                    }
                    break;
                default:
                    ResetUI();
                    break;
            }

            switch (call.State)
            {
                case CallStatePending pending:
                    StateLabel.Content = call.IsOutgoing
                        ? pending.IsReceived
                        ? Strings.Resources.VoipRinging
                        : pending.IsCreated
                        ? Strings.Resources.VoipWaiting
                        : Strings.Resources.VoipRequesting
                        : Strings.Resources.VoipIncoming;
                    break;
                case CallStateExchangingKeys exchangingKeys:
                    StateLabel.Content = Strings.Resources.VoipExchangingKeys;
                    break;
                case CallStateReady readyState:
                    // TDLib has finished its part, but the transport is still being negotiated.
                    // Only the libtgvoip controller used to advance the label past this point,
                    // so without this the modern bridge leaves it on "exchanging keys" forever.
                    if (!_modernEstablished)
                    {
                        StateLabel.Content = Strings.Resources.VoipConnecting;
                    }
                    break;
                case CallStateHangingUp hangingUp:
                    _modernEstablished = false;
                    StateLabel.Content = Strings.Resources.VoipHangingUp;
                    break;
                case CallStateDiscarded discarded:
                    _modernEstablished = false;
                    StateLabel.Content = discarded.Reason is CallDiscardReasonDeclined
                        ? Strings.Resources.VoipBusy
                        : Strings.Resources.VoipCallEnded;
                    break;
            }
        }

        private void ResetUI()
        {
            Mute.Visibility = Visibility.Visible;

            Close.Visibility = Visibility.Collapsed;
            Close.Margin = new Thickness();

            Accept.Visibility = Visibility.Collapsed;
            Accept.Margin = new Thickness();

            Discard.Margin = new Thickness();
            Discard.Visibility = Visibility.Visible;
        }

        private void OnCallStateChanged(VoIPControllerWrapper sender, libtgvoip.CallState newState)
        {
            this.BeginOnUIThread(() =>
            {
                switch (newState)
                {
                    case libtgvoip.CallState.WaitInit:
                    case libtgvoip.CallState.WaitInitAck:
                        _state = newState;
                        StateLabel.Content = Strings.Resources.VoipConnecting;
                        break;
                    case libtgvoip.CallState.Established:
                        _state = newState;
                        StateLabel.Content = "00:00";

                        SignalBarsLabel.Visibility = Visibility.Visible;
                        StartUpdatingCallDuration();
                        break;
                    case libtgvoip.CallState.Failed:
                        switch (sender.GetLastError())
                        {
                            case libtgvoip.Error.Incompatible:
                            case libtgvoip.Error.Timeout:
                            case libtgvoip.Error.Unknown:
                                _state = newState;
                                StateLabel.Content = Strings.Resources.VoipFailed;
                                break;
                        }
                        break;
                }
            });
        }

        private void OnSignalBarsChanged(VoIPControllerWrapper sender, int newCount)
        {
            this.BeginOnUIThread(() =>
            {
                SetSignalBars(newCount);
            });
        }

        public void SetSignalBars(int count)
        {
            if (_disposed)
            {
                return;
            }

            for (int i = 1; i < 5; i++)
            {
                var bar = FindName($"Signal{i}") as Rectangle;
                if (bar != null)
                {
                    bar.Fill = Resources[count >= i ? "SignalBarForegroundBrush" : "SignalBarForegroundDisabledBrush"] as SolidColorBrush;
                }
            }
        }

#if MODERN_TGCALLS
        // Modern tgcalls replacement for OnCallStateChanged. CallsService routes the bridge's
        // transport state here because VoIPControllerWrapper, which used to drive this, is
        // never created when the modern engine is in use.
        public void UpdateModernTransportState(ModernCalls.CallState state)
        {
            this.BeginOnUIThread(() =>
            {
                if (_disposed)
                {
                    return;
                }

                switch (state)
                {
                    case ModernCalls.CallState.WaitInit:
                    case ModernCalls.CallState.WaitInitAck:
                        if (!_modernEstablished)
                        {
                            StateLabel.Content = Strings.Resources.VoipConnecting;
                        }
                        break;
                    case ModernCalls.CallState.Established:
                        if (!_modernEstablished)
                        {
                            _modernEstablished = true;
                            // VoIPService._callStarted is never reset between calls, so the
                            // value passed into Update() can belong to a previous call. Take
                            // the establishment time directly instead.
                            _started = DateTime.Now;
                            StateLabel.Content = "00:00";

                            SignalBarsLabel.Visibility = Visibility.Visible;
                            StartUpdatingCallDuration();
                        }
                        break;
                    case ModernCalls.CallState.Failed:
                        _modernEstablished = false;
                        StateLabel.Content = Strings.Resources.VoipFailed;
                        break;
                }
            });
        }
#endif

        private void StartUpdatingCallDuration()
        {
            _started = _started == DateTime.MinValue ? DateTime.Now : _started;
            _durationTimer.Start();
        }

        private void DurationTimer_Tick(object sender, object e)
        {
            if (DurationLabel.Opacity == 0)
            {
                DurationLabel.Opacity = 1;
                StateLabel.Opacity = 0;
            }

            if (_state == libtgvoip.CallState.Established || _modernEstablished)
            {
                var duration = DateTime.Now - _started;
                DurationLabel.Text = duration.ToString(duration.TotalHours >= 1 ? "hh\\:mm\\:ss" : "mm\\:ss");
            }
            else
            {
                _durationTimer.Stop();
            }
        }

        private void SmallEmojiLabel_Tapped(object sender, TappedRoutedEventArgs e)
        {
            var transform = SmallPanel.TransformToVisual(LargeEmojiLabel);
            var position = transform.TransformPoint(new Point());

            _descriptionVisual.Opacity = 0;
            _largeVisual.Offset = new Vector3(position.ToVector2(), 0);
            _largeVisual.Scale = new Vector3(0.5f);
            _blurBrush.Properties.InsertScalar("Blur.BlurAmount", 0);

            var batch = _compositor.CreateScopedBatch(CompositionBatchTypes.Animation);
            var opacityAnimation = _compositor.CreateScalarKeyFrameAnimation();
            var offsetAnimation = _compositor.CreateVector3KeyFrameAnimation();
            var scaleAnimation = _compositor.CreateVector3KeyFrameAnimation();
            var blurAnimation = _compositor.CreateScalarKeyFrameAnimation();

            opacityAnimation.Duration = TimeSpan.FromMilliseconds(300);
            offsetAnimation.Duration = TimeSpan.FromMilliseconds(300);
            scaleAnimation.Duration = TimeSpan.FromMilliseconds(300);
            blurAnimation.Duration = TimeSpan.FromMilliseconds(300);

            opacityAnimation.InsertKeyFrame(1, 1);
            offsetAnimation.InsertKeyFrame(1, new Vector3(0));
            scaleAnimation.InsertKeyFrame(1, new Vector3(1));
            blurAnimation.InsertKeyFrame(1, 20);

            _descriptionVisual.StartAnimation("Opacity", opacityAnimation);
            _largeVisual.StartAnimation("Offset", offsetAnimation);
            _largeVisual.StartAnimation("Scale", scaleAnimation);
            _blurBrush.Properties.StartAnimation("Blur.BlurAmount", blurAnimation);
            _collapsed = false;

            batch.End();

            //var animation = ConnectedAnimationService.GetForCurrentView().PrepareToAnimate("EmojiAnimation", SmallEmojiLabel);
            //if (animation != null)
            //{
            //    EmojifyPanel.Visibility = Visibility.Visible;
            //    animation.TryStart(LargeEmojiLabel);
            //}
        }

        private void LargeEmojiLabel_Tapped(object sender, TappedRoutedEventArgs e)
        {
            if (_descriptionVisual.Opacity == 0)
            {
                SmallEmojiLabel_Tapped(null, null);
                return;
            }

            var transform = SmallPanel.TransformToVisual(LargeEmojiLabel);
            var position = transform.TransformPoint(new Point());

            var batch = _compositor.CreateScopedBatch(CompositionBatchTypes.Animation);
            var opacityAnimation = _compositor.CreateScalarKeyFrameAnimation();
            var offsetAnimation = _compositor.CreateVector3KeyFrameAnimation();
            var scaleAnimation = _compositor.CreateVector3KeyFrameAnimation();
            var blurAnimation = _compositor.CreateScalarKeyFrameAnimation();

            opacityAnimation.Duration = TimeSpan.FromMilliseconds(300);
            offsetAnimation.Duration = TimeSpan.FromMilliseconds(300);
            scaleAnimation.Duration = TimeSpan.FromMilliseconds(300);
            blurAnimation.Duration = TimeSpan.FromMilliseconds(300);

            opacityAnimation.InsertKeyFrame(1, 0);
            offsetAnimation.InsertKeyFrame(1, new Vector3(position.ToVector2(), 0));
            scaleAnimation.InsertKeyFrame(1, new Vector3(0.5f));
            blurAnimation.InsertKeyFrame(1, 0);

            _descriptionVisual.StartAnimation("Opacity", opacityAnimation);
            _largeVisual.StartAnimation("Offset", offsetAnimation);
            _largeVisual.StartAnimation("Scale", scaleAnimation);
            _blurBrush.Properties.StartAnimation("Blur.BlurAmount", blurAnimation);
            _collapsed = true;

            batch.End();
        }

        private void Close_Click(object sender, RoutedEventArgs e)
        {
            _aggregator.Publish(new UpdateCall(new Call { State = new CallStateDiscarded { Reason = new CallDiscardReasonEmpty() } }));
        }

        private async void Accept_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                Function request;
                BaseObject response;
                if (_call.IsOutgoing && _call.State is CallStateDiscarded discarded && discarded.Reason is CallDiscardReasonDeclined)
                {
                    request = ModernTdlibCompatibility.CreateCall(_call.UserId, ModernTdlibCompatibility.CreateAudioCallProtocol());
                    response = await _protoService.SendAsync(request);
                    ModernTdlibCompatibility.LogAudioCallRequestResult("retry", response);
                    return;
                }

                request = ModernTdlibCompatibility.CreateAcceptCall(_call.Id, ModernTdlibCompatibility.CreateAudioCallProtocol());
                response = await _protoService.SendAsync(request);
                ModernTdlibCompatibility.LogAudioCallRequestResult("accept", response);
            }
            catch (Exception ex)
            {
                ModernTdlibCompatibility.LogAudioCallRequestException("accept_exception", ex);
                await MessagePopup.ShowAsync(Strings.Resources.VoipFailed, Strings.Resources.AppName, Strings.Resources.OK);
            }
        }

        private void Hangup_Click(object sender, RoutedEventArgs e)
        {
            var call = _call;
            if (call == null)
            {
                return;
            }

            var relay = 0L;
            if (_controller != null)
            {
                relay = _controller.GetPreferredRelayID();
            }

            var duration = _state == libtgvoip.CallState.Established || _modernEstablished ? DateTime.Now - _started : TimeSpan.Zero;
            _protoService.Send(ModernTdlibCompatibility.CreateDiscardCall(call.Id, false, (int)duration.TotalSeconds, relay));
        }

        private void Routing_Click(object sender, RoutedEventArgs e)
        {
            var toggle = sender as ToggleButton;
            var routingManager = _audioRoutingManager;
            if (toggle == null || routingManager == null)
            {
                return;
            }

            toggle.IsChecked = !toggle.IsChecked;

            if (toggle.IsChecked.Value)
            {
                routingManager.SetAudioEndpoint(AudioRoutingEndpoint.Speakerphone);
            }
            else
            {
                if (routingManager.AvailableAudioEndpoints.HasFlag(AvailableAudioRoutingEndpoints.Bluetooth))
                {
                    routingManager.SetAudioEndpoint(AudioRoutingEndpoint.Bluetooth);
                }
                else if (routingManager.AvailableAudioEndpoints.HasFlag(AvailableAudioRoutingEndpoints.Earpiece))
                {
                    routingManager.SetAudioEndpoint(AudioRoutingEndpoint.Earpiece);
                }
            }
        }

        private bool _isMuted;
        public bool IsMuted
        {
            get
            {
                return _isMuted;
            }
            set
            {
                _isMuted = value;

                if (_controller != null)
                {
                    _controller.SetMicMute(value);
                }
#if MODERN_TGCALLS
                else
                {
                    // The modern bridge owns the microphone instead of VoIPControllerWrapper,
                    // and CallsService owns the session, so the toggle is forwarded there.
                    ModernMuteRequested?.Invoke(value);
                }
#endif
            }
        }

#if MODERN_TGCALLS
        /// <summary>
        /// Supplied by CallsService so the mute toggle can reach the modern tgcalls session.
        /// </summary>
        public Action<bool> ModernMuteRequested { get; set; }
#endif

        private async void AudioEndpointChanged(AudioRoutingManager sender, object args)
        {
            if (_disposed)
            {
                return;
            }

            await Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () =>
            {
                if (!_disposed && Routing != null && sender != null)
                {
                    Routing.IsChecked = sender.GetAudioEndpoint() == AudioRoutingEndpoint.Speakerphone;
                }
            });
        }

        private void DebugString_Tapped(object sender, TappedRoutedEventArgs e)
        {
            if (_debugTapped == 9)
            {
                _debugTapped = 0;
                ShowDebugString();
            }
            else
            {
                _debugTapped++;
            }
        }

        private async void ShowDebugString()
        {
            if (_controller == null)
            {
                return;
            }

            var debug = _controller.GetDebugString();
            var version = VoIPControllerWrapper.GetVersion();

            var text = new TextBlock();
            text.Text = debug;
            text.Margin = new Thickness(12, 16, 12, 0);
            text.Style = Application.Current.Resources["BodyTextBlockStyle"] as Style;

            var scroll = new ScrollViewer();
            scroll.VerticalScrollBarVisibility = ScrollBarVisibility.Auto;
            scroll.VerticalScrollMode = ScrollMode.Auto;
            scroll.Content = text;

            var dialog = new ContentPopup();
            dialog.Title = $"libtgvoip v{version}";
            dialog.Content = scroll;
            dialog.PrimaryButtonText = "OK";
            dialog.Closed += (s, args) =>
            {
                _debugDialog = null;
                _debugTimer.Stop();
            };

            _debugDialog = dialog;
            _debugTimer.Start();

            await dialog.ShowQueuedAsync();
        }

        private void DebugTimer_Tick(object sender, object e)
        {
            if (_debugDialog == null || _controller == null)
            {
                _debugTimer.Stop();
                return;
            }

            if (_debugDialog.Content is ScrollViewer scroll && scroll.Content is TextBlock text)
            {
                text.Text = _controller.GetDebugString();
            }
        }
    }
}
