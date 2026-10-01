using System.Numerics;
using Windows.UI;
using Windows.UI.Composition;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Hosting;
using Windows.UI.Xaml.Shapes;

namespace Unigram.Common
{
    public static class DropShadowEx
    {
#if MODERN_TDLIB
        // Attach is called from many controls, and the diagnostics log is deleted once it
        // reaches its size cap, so unbounded tracing here would evict the startup records
        // this instrumentation exists to capture. Trace only the first few calls, which
        // covers the MainPage constructor.
        private static int _traceBudget = 4;

        private static bool ShouldTrace()
        {
            return System.Threading.Interlocked.Decrement(ref _traceBudget) >= 0;
        }

        private static void Trace(bool enabled, string details)
        {
            if (enabled)
            {
                Unigram.Logs.PushDiagnostics.Write("shadow.attach", details);
            }
        }
#endif

        public static Visual Attach(UIElement element, float radius, float opacity, CompositionClip clip = null)
        {
#if MODERN_TDLIB
            var trace = ShouldTrace();
            Trace(trace, $"step=element;element={(element == null ? "null" : "ok")}");
#endif
            var elementVisual = ElementCompositionPreview.GetElementVisual(element);
#if MODERN_TDLIB
            Trace(trace, $"step=element_visual;visual={(elementVisual == null ? "null" : "ok")};compositor={(elementVisual?.Compositor == null ? "null" : "ok")}");
#endif

            var shadow = elementVisual.Compositor.CreateDropShadow();
            shadow.BlurRadius = radius;
            shadow.Opacity = opacity;
            shadow.Color = Colors.Black;
#if MODERN_TDLIB
            Trace(trace, $"step=shadow;shadow={(shadow == null ? "null" : "ok")}");
#endif

            var visual = elementVisual.Compositor.CreateSpriteVisual();
#if MODERN_TDLIB
            Trace(trace, $"step=sprite;sprite={(visual == null ? "null" : "ok")}");
#endif
            visual.Shadow = shadow;
            visual.Size = new Vector2(0, 0);
            visual.Offset = new Vector3(0, 0, 0);
            visual.Clip = clip;

            switch (element)
            {
                case Image image:
                    shadow.Mask = image.GetAlphaMask();
                    break;
                case Shape shape:
                    shadow.Mask = shape.GetAlphaMask();
                    break;
                case TextBlock textBlock:
                    shadow.Mask = textBlock.GetAlphaMask();
                    break;
            }

            ElementCompositionPreview.SetElementChildVisual(element, visual);
#if MODERN_TDLIB
            Trace(trace, "step=done");
#endif
            return visual;
        }
    }
}
