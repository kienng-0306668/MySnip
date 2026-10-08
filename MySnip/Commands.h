#pragma once

// Mã lệnh của các nút. 0 dành cho "không có gì", nên ID bắt đầu từ 1.
enum Command
{
    CMD_NONE = 0,

    // Công cụ chú thích
    CMD_TOOL_RECT,      // chữ nhật / elip
    CMD_TOOL_ARROW,
    CMD_TOOL_PEN,
    CMD_TOOL_MARKER,
    CMD_TOOL_MOSAIC,
    CMD_TOOL_TEXT,
    CMD_TOOL_ERASER,

    // Lịch sử
    CMD_UNDO,
    CMD_REDO,

    // Hành động
    CMD_CANCEL,
    CMD_PIN,
    CMD_SAVE,
    CMD_COPY,
};

// Icon glyph dùng cho toolbar button
enum ToolbarIcon
{
    IC_NONE = 0,
    IC_UNDO,
    IC_RECT,
    IC_REDO,
    IC_ELLIPSE,
    IC_ARROW,
    IC_PEN,
    IC_MARKER,
    IC_MOSAIC,
    IC_TEXT,
    IC_ERASER,
    IC_CLOSE,
    IC_PIN,
    IC_SAVE,
    IC_COPY,
};