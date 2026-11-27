// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Windows;

namespace P4SyncGui.Services;

/// <summary>Swaps the first merged dictionary (colour tokens only) at runtime.</summary>
public static class ThemeService
{
    public static void Apply(string? theme)
    {
        var app = Application.Current;
        if (app is null) return;

        string source = string.Equals(theme, "Light", StringComparison.OrdinalIgnoreCase)
            ? "Themes/LightTheme.xaml"
            : "Themes/DarkTheme.xaml";

        var dictionaries = app.Resources.MergedDictionaries;
        var old = dictionaries.FirstOrDefault(d =>
            d.Source is not null &&
            d.Source.OriginalString.Contains("Theme.xaml", StringComparison.OrdinalIgnoreCase));
        if (old is not null)
            dictionaries.Remove(old);
        dictionaries.Insert(0, new ResourceDictionary { Source = new Uri(source, UriKind.Relative) });
    }
}
