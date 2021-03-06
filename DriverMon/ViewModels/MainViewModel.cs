﻿using Prism.Commands;
using Prism.Mvvm;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Input;
using Zodiacon.WPF;
using System.Windows;
using System.ServiceProcess;
using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using System.Diagnostics;
using DriverMon.Views;
using System.Windows.Data;
using Syncfusion.Data;
using MahApps.Metro;

namespace DriverMon.ViewModels {
    class MainViewModel : BindableBase, IDisposable {
        DriverInterface _driver;
        readonly IUIServices UI;
        readonly ObservableCollection<IrpArrivedViewModel> _requests = new ObservableCollection<IrpArrivedViewModel>();
        readonly Dictionary<IntPtr, DriverViewModel> _driversd = new Dictionary<IntPtr, DriverViewModel>(8);
        List<DriverViewModel> _drivers;

        public ObservableCollection<IrpArrivedViewModel> Requests => _requests;
        public IList<DriverViewModel> Drivers => _drivers;

        AccentViewModel[] _accents;
        public AccentViewModel[] Accents => _accents ?? (_accents = ThemeManager.Accents.Select(accent => new AccentViewModel(accent)).ToArray());
        AccentViewModel _currentAccent;
        public AccentViewModel CurrentAccent => _currentAccent;

        public ICommand ChangeAccentCommand => new DelegateCommand<AccentViewModel>(accent => {
            if (_currentAccent != null)
                _currentAccent.IsCurrent = false;
            _currentAccent = accent;
            //_settings.AccentName = _currentAccent.Name;
            accent.IsCurrent = true;
            RaisePropertyChanged(nameof(CurrentAccent));
        }, accent => accent != _currentAccent).ObservesProperty(() => CurrentAccent);

        public bool IsAlwaysOnTop {
            get => Application.Current.MainWindow.Topmost;
            set => Application.Current.MainWindow.Topmost = value;
        }

        public MainViewModel(IUIServices ui) {
            UI = ui;
        }

        public ICommand OnLoad => new DelegateCommand(async () => await StartDriver());

        public ICommand StartMonitoringCommand => new DelegateCommand(() => {
            var driver = _driver.AddDriver(@"\driver\disk");
            if (driver == IntPtr.Zero) {
                UI.MessageBoxService.ShowMessage($"Error: {Marshal.GetLastWin32Error()}", App.Title);
                return;
            }
            _driver.StartMonitoring();
        });

        private async Task InstallAndLoadDriverAsync() {
            var status = await DriverInterface.LoadDriverAsync("DriverMon");
            if (status == null) {
                var ok = await DriverInterface.InstallDriverAsync("DriverMon", Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location) + @"\DriverMonitor.sys");
                if (!ok) {
                    UI.MessageBoxService.ShowMessage("Failed to install driver. Exiting", App.Title);
                    Application.Current.Shutdown(1);
                }
                status = await DriverInterface.LoadDriverAsync("DriverMon");
            }
            if (status != ServiceControllerStatus.Running) {
                UI.MessageBoxService.ShowMessage("Failed to start driver. Exiting", App.Title);
                Application.Current.Shutdown(1);
            }
        }

        private async Task StartDriver() {
            if (_driver != null)
                return;

            try {
                _driver = new DriverInterface();
            }
            catch (Win32Exception ex) when (ex.NativeErrorCode == 2) {
                await InstallAndLoadDriverAsync();
                _driver = new DriverInterface();
            }
            catch (Exception ex) {
                UI.MessageBoxService.ShowMessage($"Error: {ex.Message}", App.Title);
                Application.Current.Shutdown(1);
            }
        }

        public void Dispose() {
            _driver.Dispose();
        }

        public unsafe void Update(byte[] data, int size) {
            fixed (byte* bytes = data) {
                var p = bytes;
                do {
                    IrpArrivedInfoBase* info = (IrpArrivedInfoBase*)p;
                    _requests.Add(new IrpArrivedViewModel(_requests.Count + 1, _driversd[info->DriverObject].Name, info));
                    size -= info->Header.Size;
                    p += info->Header.Size;
                } while (size > sizeof(CommonInfoHeader));
            }

            if (AutoScroll) {
            }
        }

        public ICollectionViewAdv View { get; set; }

        public ICommand DriversSettingsCommand => new DelegateCommand(() => {
            var vm = UI.DialogService.CreateDialog<DriversSettingsViewModel, DriversSettingsDialog>(Drivers);
            if (vm.ShowDialog() == true) {
                _drivers = vm.Drivers;
                _driver.RemoveAllDrivers();
                _driversd.Clear();

                foreach (var driver in _drivers.Where(d => d.IsMonitored)) {
                    string driverName = @"\driver\" + driver.Name;
                    var result = _driver.AddDriver(driverName);
                    if (result == IntPtr.Zero) {
                        UI.MessageBoxService.ShowMessage($"Failed to hook driver {driver.Name}", App.Title);
                    }
                    else {
                        _driversd.Add(result, driver);
                    }
                }
            }
        });

        bool _isMonitoring;
        public bool IsMonitoring {
            get => _isMonitoring;
            set {
                if (SetProperty(ref _isMonitoring, value)) {
                    if (value)
                        _driver.StartMonitoring();
                    else
                        _driver.StopMonitoring();
                }
            }
        }

        bool _autoScroll;
        public bool AutoScroll {
            get => _autoScroll;
            set => SetProperty(ref _autoScroll, value);
        }

        string _searchText;
        public string SearchText {
            get => _searchText;
            set {
                if (SetProperty(ref _searchText, value)) {
                    if (string.IsNullOrWhiteSpace(value)) {
                        View.Filter = null;
                    }
                    else {
                        var text = value.ToLowerInvariant();
                        View.Filter = obj => {
                            var request = (IrpArrivedViewModel)obj;
                            return request.DriverName.Contains(text) || request.MajorCode.ToString().ToLowerInvariant().Contains(text)
                                || request.ProcessName?.ToLowerInvariant().Contains(text) == true;
                        };
                    }
                    View.RefreshFilter(true);
                }
            }
        }
    }
}
