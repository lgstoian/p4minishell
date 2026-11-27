// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Windows.Controls;
using System.Windows.Input;
using P4SyncGui.ViewModels;

namespace P4SyncGui.Views;

public partial class ShellView : UserControl
{
    public ShellView() => InitializeComponent();

    private void CommandBox_KeyDown(object sender, KeyEventArgs e)
    {
        if (DataContext is not ShellViewModel vm) return;
        if (e.Key == Key.Up) { vm.HistoryBack(); e.Handled = true; }
        else if (e.Key == Key.Down) { vm.HistoryForward(); e.Handled = true; }
    }
}
