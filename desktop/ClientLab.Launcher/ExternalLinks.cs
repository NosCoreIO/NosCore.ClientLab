using System.Diagnostics;
using Xilium.CefGlue;
using Xilium.CefGlue.Avalonia;
using Xilium.CefGlue.Common.Handlers;

namespace NosCore.ClientLab.Launcher;

/// <summary>
/// Web pages the embedded browsers would open in a new window (window.open, target=_blank, the client's own
/// web-page calls) go to the user's default browser instead of a bare Chromium window.
/// </summary>
public static class ExternalLinks
{
    /// <summary>Opens an http(s) address in the default browser; other schemes are refused.</summary>
    public static bool Open(string? url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var address) || address.Scheme is not ("http" or "https")) return false;
        try { Process.Start(new ProcessStartInfo(address.AbsoluteUri) { UseShellExecute = true }); return true; }
        catch (Exception) { return false; }
    }

    public static AvaloniaCefBrowser OpeningPopupsExternally(this AvaloniaCefBrowser browser)
    {
        browser.LifeSpanHandler = new PopupsToDefaultBrowser();
        return browser;
    }

    private sealed class PopupsToDefaultBrowser : LifeSpanHandler
    {
        protected override bool OnBeforePopup(CefBrowser browser, CefFrame frame, string targetUrl, string targetFrameName,
            CefWindowOpenDisposition targetDisposition, bool userGesture, CefPopupFeatures popupFeatures, CefWindowInfo windowInfo,
            ref CefClient client, CefBrowserSettings settings, ref CefDictionaryValue extraInfo, ref bool noJavascriptAccess)
        {
            Open(targetUrl);
            return true; // never create the Chromium popup itself
        }
    }
}
