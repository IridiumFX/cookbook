@echo off
title Cookbook Registry Server
echo ============================================
echo   Cookbook Artifact Registry
echo ============================================
echo.

:: Configuration — edit these as needed
set COOKBOOK_PORT=8080
set COOKBOOK_REGISTRY_ID=central
set COOKBOOK_KEY_DIR=.\keys
set COOKBOOK_DB_URL=cookbook.db
set COOKBOOK_STORAGE_DIR=.\data\objects
set COOKBOOK_JWT_TTL_SEC=3600
set COOKBOOK_RATE_LIMIT_PER_MIN=0
set COOKBOOK_PENDING_TIMEOUT_SEC=3600
set COOKBOOK_AUDIT_LOG=.\data\audit.pasta
set COOKBOOK_OBJECT_CACHE_TTL_SEC=86400

:: Runtime libraries
set PATH=C:\Program Files\PostgreSQL\16\bin;C:\Program Files\JetBrains\CLion 2025.3.3\bin\mingw\bin;%PATH%

:: Create directories if needed
if not exist keys mkdir keys
if not exist data\objects mkdir data\objects

:: Run the server
echo Starting on http://localhost:%COOKBOOK_PORT% (auth: enabled)
echo Press Ctrl+C to stop.
echo.
build\bin\cookbook_server.exe

pause
