// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
using System.Windows;
using System.Windows.Controls;
using P4SyncGui.ViewModels;

namespace P4SyncGui.Views;

public partial class SettingsView : UserControl
{
    public SettingsView() => InitializeComponent();

    private void SaveCredential_Click(object sender, RoutedEventArgs e)
    {
        if (DataContext is not SettingsViewModel vm) return;
        string store = vm.SaveCredential(CredentialUserBox.Text, CredentialPasswordBox.Password);
        CredentialPasswordBox.Clear(); // the secret stays only in the OS store
        CredentialStatus.Text = "Saved in " + store + " (value hidden)";
    }
}
