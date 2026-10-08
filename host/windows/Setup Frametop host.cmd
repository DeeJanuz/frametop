@echo off
rem Frametop host setup: makes this PC a host for Frametop's remote displays (frametop-host-setup.ps1).
rem It asks Windows for admin.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0frametop-host-setup.ps1" %*
