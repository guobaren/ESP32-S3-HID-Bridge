namespace HidBridge.Host.Transport;

internal readonly record struct SerialPortStatus(string PortName, int BaudRate, bool LegacySingleBoard = false);
