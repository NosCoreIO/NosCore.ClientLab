using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;
using NosCore.ClientLab.Launcher.ViewModels;

internal static class NewsChecks
{
    public static void Run()
    {
        var entries = new JsonArray();
        for (var index = 0; index < 5; index++)
            entries.Add(new JsonObject { ["id"] = $"00000000-0000-0000-0000-{index:000000000000}", ["title"] = "Official article " + index,
                ["imageSmall"] = index == 1 ? null : "https://secure-asset-delivery.gameforge.com/card-444x250.jpg",
                ["imageBig"] = "https://secure-asset-delivery.gameforge.com/banner-895x300.jpg",
                ["publishDate"] = "2026-11-14T10:00:00Z", ["showInLibrary"] = 1, ["showAsEvent"] = index == 2 ? 1 : 0, ["link"] = "" });
        var news = OfficialNews.Parse(entries.ToJsonString());
        if (news.Length != 5 || news[0].ImageUrl != "https://secure-asset-delivery.gameforge.com/card-444x250.jpg" ||
            news[1].ImageUrl != OfficialNews.FallbackImage || news[2].Category != "EVENT" ||
            news.Any(item => !OfficialNews.IsOfficialLink(item.Url) || item.ImageUrl is null))
            throw new Exception("Official product news lost its card images, fallback, events or links.");
        var view = new MainViewModel();
        foreach (var page in news.Chunk(4))
            view.NewsPages.Add(new MainViewModel.NewsPage(page.Select(entry => new MainViewModel.NewsCard(entry) { LoadingImage = true }).ToArray()));
        view.PreviousNewsCommand.Execute(null);
        if (view.NewsPageIndex != 0 || view.CanPreviousNews || !view.CanNextNews) throw new Exception("News paging did not start at the first page.");
        view.NextNewsCommand.Execute(null); view.NextNewsCommand.Execute(null);
        if (view.NewsPageIndex != 1 || view.CanNextNews || !view.CanPreviousNews || view.NewsPageLabel != "2 / 2" || view.NewsPages[1].Items.Count != 1)
            throw new Exception("News paging skipped the final partial page or exceeded its bounds.");
        view.PreviousNewsCommand.Execute(null);
        if (view.NewsPageIndex != 0) throw new Exception("Previous news did not return to the first page.");
        Console.WriteLine("PASS: the full product-news feed retains card images, fallbacks, events and article links for paging.");
    }
}
