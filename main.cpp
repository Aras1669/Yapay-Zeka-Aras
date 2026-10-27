#include "llama.h"
#include "raylib.h"
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <random>
#include <cmath>
#include <deque>
#include <sstream>
#include <iomanip>

enum AppState {
    STATE_MODEL_CONFIRM,
    STATE_LOADING,
    STATE_CHAT,
    STATE_SETTINGS
};

struct ChatMessage {
    std::string sender;
    std::string text;
};

// Yükleme iptal ve akıcı ilerleme takibi için veri yapısı
struct LoadUserData {
    bool cancelled = false;
    float display_progress = 0.0f;
};

static void quiet_log_callback(enum ggml_log_level level, const char * text, void * user_data) {
    // Susturuldu
}

Font customFont;

// Yumuşak ve yüksek kontrastlı renk geçişi (Lerp) fonksiyonu
Color ColorLerpCustom(Color c1, Color c2, float t) {
    return Color{
        static_cast<unsigned char>(c1.r + (c2.r - c1.r) * t),
        static_cast<unsigned char>(c1.g + (c2.g - c1.g) * t),
        static_cast<unsigned char>(c1.b + (c2.b - c1.b) * t),
        static_cast<unsigned char>(c1.a + (c2.a - c1.a) * t)
    };
}

// Ondalıklı sayıları gereksiz sıfırlardan arındırıp sade gösterme fonksiyonu (örn: 0.700000 -> 0.7)
std::string format_float(float val) {
    std::ostringstream stream;
    stream << val;
    return stream.str();
}

void encode_utf8(int codepoint, std::string& out) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::vector<std::string> WrapText(Font font, const std::string& text, float maxWidth, float fontSize, float spacing) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string word, currentLine;
    
    while (stream >> word) {
        std::string testLine = currentLine.empty() ? word : currentLine + " " + word;
        if (MeasureTextEx(font, testLine.c_str(), fontSize, spacing).x > maxWidth) {
            if (!currentLine.empty()) {
                lines.push_back(currentLine);
                currentLine = word;
            } else {
                lines.push_back(word);
                currentLine = "";
            }
        } else {
            currentLine = testLine;
        }
    }
    if (!currentLine.empty()) {
        lines.push_back(currentLine);
    }
    if (lines.empty()) {
        lines.push_back("");
    }
    return lines;
}

void DrawTextCenteredInBox(Font font, const char* text, Rectangle box, float fontSize, float spacing, Color tint) {
    Vector2 textSize = MeasureTextEx(font, text, fontSize, spacing);
    float x = box.x + (box.width - textSize.x) / 2.0f;
    float y = box.y + (box.height - textSize.y) / 2.0f;
    DrawTextEx(font, text, Vector2{x, y}, fontSize, spacing, tint);
}

// Tamamen sabit ve akıcı hızda (linear step) ilerleyen, pencerene göre ölçeklenen callback
bool model_progress_callback(float progress, void* user_data) {
    LoadUserData* data = static_cast<LoadUserData*>(user_data);
    if (!data) return true;
    
    if (WindowShouldClose() || IsKeyPressed(KEY_ESCAPE)) {
        data->cancelled = true;
        return false;
    }

    float linearStep = 0.008f;

    while (data->display_progress < progress && !data->cancelled) {
        if (WindowShouldClose() || IsKeyPressed(KEY_ESCAPE)) {
            data->cancelled = true;
            return false;
        }

        data->display_progress += linearStep;
        if (data->display_progress > progress) {
            data->display_progress = progress;
        }

        int percent = static_cast<int>(data->display_progress * 100);
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        
        BeginDrawing();
        ClearBackground(Color{20, 20, 25, 255});
        
        DrawTextCenteredInBox(customFont, "ARAS'IN YAPAY ZEKA ASISTANI", Rectangle{0, static_cast<float>(sh) * 0.35f, static_cast<float>(sw), 30}, 22, 1.0f, RAYWHITE);
        DrawTextCenteredInBox(customFont, "Model yükleniyor, lütfen bekleyin...", Rectangle{0, static_cast<float>(sh) * 0.35f + 40, static_cast<float>(sw), 20}, 16, 1.0f, LIGHTGRAY);
        
        int barWidth = 400;
        int barHeight = 25;
        int barX = (sw - barWidth) / 2;
        int barY = static_cast<int>(sh * 0.35f) + 90;
        
        DrawRectangle(barX, barY, barWidth, barHeight, Color{40, 40, 50, 255});
        DrawRectangle(barX, barY, static_cast<int>(barWidth * data->display_progress), barHeight, Color{0, 220, 120, 255});
        DrawRectangleLines(barX, barY, barWidth, barHeight, DARKGRAY);
        
        std::string pctText = "%" + std::to_string(percent);
        DrawTextCenteredInBox(customFont, pctText.c_str(), Rectangle{static_cast<float>(barX), static_cast<float>(barY), static_cast<float>(barWidth), static_cast<float>(barHeight)}, 18, 1.0f, RAYWHITE);

        // İptal Butonu
        Rectangle cancelButton = { static_cast<float>((sw - 300) / 2), static_cast<float>(barY + 60), 300, 40 };
        bool cancelHover = CheckCollisionPointRec(GetMousePosition(), cancelButton);
        DrawRectangleRec(cancelButton, cancelHover ? Color{210, 70, 70, 255} : Color{160, 40, 40, 255});
        DrawTextCenteredInBox(customFont, "İptal Et (ESC)", cancelButton, 14, 1.0f, RAYWHITE);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && cancelHover) {
            data->cancelled = true;
            EndDrawing();
            return false;
        }

        EndDrawing();

        if (data->display_progress >= progress) break;
    }

    return !data->cancelled;
}

std::string format_chatml(const std::string& user_input, const std::string& think_instr) {
    return "<|im_start|>system\n" + think_instr + "<|im_end|>\n<|im_start|>user\n" + user_input + "<|im_end|>\n<|im_start|>assistant\n";
}

llama_token sample_with_temperature(float* logits, int n_vocab, float temperature, float repeat_penalty, const std::deque<llama_token>& last_tokens) {
    if (repeat_penalty != 1.0f && !last_tokens.empty()) {
        for (auto token : last_tokens) {
            if (token >= 0 && token < n_vocab) {
                float l = logits[token];
                logits[token] = (l < 0.0f) ? l * repeat_penalty : l / repeat_penalty;
            }
        }
    }

    if (temperature <= 0.01f) {
        int best_i = 0;
        float max_l = -1e9f;
        for (int i = 0; i < n_vocab; ++i) {
            if (logits[i] > max_l) {
                max_l = logits[i];
                best_i = i;
            }
        }
        return best_i;
    }

    std::vector<float> probs(n_vocab);
    float max_l = -1e9f;
    for (int i = 0; i < n_vocab; ++i) {
        if (logits[i] > max_l) max_l = logits[i];
    }

    float sum = 0.0f;
    for (int i = 0; i < n_vocab; ++i) {
        probs[i] = std::exp((logits[i] - max_l) / temperature);
        sum += probs[i];
    }

    for (int i = 0; i < n_vocab; ++i) {
        probs[i] /= sum;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    float r = dis(gen);

    float cumul = 0.0f;
    for (int i = 0; i < n_vocab; ++i) {
        cumul += probs[i];
        if (r <= cumul) return i;
    }
    return n_vocab - 1;
}

int main(int argc, char** argv) {
    // Pencerenin boyutlandırılabilir (Resizable) olmasını sağlıyoruz
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_WINDOW_RESIZABLE);

    const int initialWidth = 1000;
    const int initialHeight = 750;
    InitWindow(initialWidth, initialHeight, "Aras'ın Yapay Zeka Asistanı - Raylib");
    
    SetExitKey(0); // ESC ile kapanmayı engelle
    SetTargetFPS(60);

    std::vector<int> codepoints;
    for (int i = 32; i <= 126; i++) {
        codepoints.push_back(i);
    }
    int turkish_codepoints[] = { 199, 231, 286, 287, 304, 305, 214, 246, 350, 351, 220, 252 };
    for (int cp : turkish_codepoints) {
        codepoints.push_back(cp);
    }

    customFont = LoadFontEx("/System/Library/Fonts/Supplemental/Arial.ttf", 64, codepoints.data(), static_cast<int>(codepoints.size()));
    if (customFont.texture.id == 0) {
        customFont = GetFontDefault();
    }
    SetTextureFilter(customFont.texture, TEXTURE_FILTER_BILINEAR);

    llama_backend_init();
    llama_log_set(quiet_log_callback, nullptr);

    std::filesystem::path exe_path = std::filesystem::absolute(argv[0]);
    std::string exec_dir = exe_path.parent_path().string();

    std::string external_model_path = "";
    std::string internal_model_path = "";

    // 1. Dışarıda herhangi bir .gguf dosyası arama
    std::filesystem::path search_dir;
    auto parent = exe_path.parent_path();
    if (parent.filename() == "MacOS" && parent.parent_path().filename() == "Contents") {
        auto app_bundle_dir = parent.parent_path().parent_path();
        search_dir = app_bundle_dir.parent_path();
    } else {
        search_dir = std::filesystem::current_path();
    }

    if (std::filesystem::exists(search_dir) && std::filesystem::is_directory(search_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(search_dir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".gguf" || ext == ".GGUF") {
                    external_model_path = entry.path().string();
                    break;
                }
            }
        }
    }

    // 2. İçeride dahili model.gguf kontrolü
    std::filesystem::path internal_candidate = exe_path.parent_path() / "model.gguf";
    if (std::filesystem::exists(internal_candidate)) {
        internal_model_path = internal_candidate.string();
    }

    AppState appState;
    std::string model_path = "";

    if (!external_model_path.empty()) {
        appState = STATE_MODEL_CONFIRM;
    } else if (!internal_model_path.empty()) {
        model_path = internal_model_path;
        appState = STATE_LOADING;
    } else {
        model_path = "model.gguf";
        appState = STATE_LOADING;
    }

    std::string think_setting = "medium";
    std::string creativity_setting = "medium";
    std::string repeat_penalty_setting = "medium";

    float temperature = 0.7f;
    float repeat_penalty = 1.3f;
    int max_output_tokens = 4096;
    
    std::string think_instruction = "Sen Qwen veya Alibaba değil, Aras tarafından geliştirilen 'Aras'ın Yapay Zeka Asistanı'sın. Sana adın veya kim olduğun sorulduğunda asla Qwen olduğunu söyleme, kesinlikle 'Aras'ın Yapay Zeka Asistanı' olduğunu belirt. Standart bir şekilde akıl yürüt.";

    llama_model * model = nullptr;
    llama_context * ctx = nullptr;
    llama_batch batch;
    int n_batch = 512;
    int current_context_position = 0;

    std::vector<ChatMessage> chatHistory;
    std::string userInputStr = "";
    bool isGenerating = false;
    int genStep = 0;
    std::deque<llama_token> last_tokens;
    const size_t penalty_window = 64;

    LoadUserData loadUserData;
    const struct llama_vocab * vocab = nullptr;

    float editBtnHoverProg = 0.0f;
    float sendBtnHoverProg = 0.0f;
    float confirmYesHoverProg = 0.0f;
    float confirmNoHoverProg = 0.0f;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        float hoverAnimSpeed = 18.0f;

        // Her karede anlık pencere boyutlarını alıyoruz (Responsive Tasarım)
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();

        // --- DURUM 1: DIŞ ÇEVREDE BULUNAN HERHANGİ BİR .GGUF İÇİN ONAY EKRANI ---
        if (appState == STATE_MODEL_CONFIRM) {
            BeginDrawing();
            ClearBackground(Color{20, 20, 25, 255});

            DrawTextCenteredInBox(customFont, "DIŞARIDA MODEL TESPİT EDİLDİ", Rectangle{0, static_cast<float>(sh) * 0.25f, static_cast<float>(sw), 30}, 22, 1.0f, RAYWHITE);
            DrawTextCenteredInBox(customFont, "Uygulamanın dışında şu GGUF dosyası bulundu:", Rectangle{0, static_cast<float>(sh) * 0.25f + 50, static_cast<float>(sw), 20}, 16, 1.0f, LIGHTGRAY);
            DrawTextCenteredInBox(customFont, std::filesystem::path(external_model_path).filename().string().c_str(), Rectangle{0, static_cast<float>(sh) * 0.25f + 90, static_cast<float>(sw), 25}, 18, 1.0f, YELLOW);
            DrawTextCenteredInBox(customFont, "Bu dosya ile açılsın mı?", Rectangle{0, static_cast<float>(sh) * 0.25f + 140, static_cast<float>(sw), 20}, 16, 1.0f, RAYWHITE);

            Rectangle yesButton = { static_cast<float>(sw / 2 - 160), static_cast<float>(sh * 0.25f + 200), 150, 45 };
            Rectangle noButton = { static_cast<float>(sw / 2 + 10), static_cast<float>(sh * 0.25f + 200), 160, 45 };

            bool yesHover = CheckCollisionPointRec(GetMousePosition(), yesButton);
            bool noHover = CheckCollisionPointRec(GetMousePosition(), noButton);

            confirmYesHoverProg += ((yesHover ? 1.0f : 0.0f) - confirmYesHoverProg) * hoverAnimSpeed * dt;
            confirmNoHoverProg += ((noHover ? 1.0f : 0.0f) - confirmNoHoverProg) * hoverAnimSpeed * dt;

            Color yesColor = ColorLerpCustom(Color{0, 140, 70, 255}, Color{0, 200, 110, 255}, confirmYesHoverProg);
            Color noColor = ColorLerpCustom(Color{170, 40, 40, 255}, Color{220, 80, 80, 255}, confirmNoHoverProg);

            DrawRectangleRec(yesButton, yesColor);
            DrawTextCenteredInBox(customFont, "Evet (Bu Dosyayla)", yesButton, 12, 1.0f, RAYWHITE);

            DrawRectangleRec(noButton, noColor);
            DrawTextCenteredInBox(customFont, "Hayır (İçeriği Kullan)", noButton, 12, 1.0f, RAYWHITE);

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (yesHover) {
                    model_path = external_model_path;
                    appState = STATE_LOADING;
                } else if (noHover) {
                    model_path = !internal_model_path.empty() ? internal_model_path : "model.gguf";
                    appState = STATE_LOADING;
                }
            }

            EndDrawing();
            continue;
        }

        // --- DURUM 2: MODEL YÜKLEME AŞAMASI ---
        if (appState == STATE_LOADING && model == nullptr) {
            llama_model_params model_params = llama_model_default_params();
            model_params.n_gpu_layers = 99;
            model_params.progress_callback = model_progress_callback;
            model_params.progress_callback_user_data = &loadUserData;

            model = llama_model_load_from_file(model_path.c_str(), model_params);

            if (!model) {
                if (loadUserData.cancelled) {
                    UnloadFont(customFont);
                    CloseWindow();
                    return 0;
                }

                while (!WindowShouldClose()) {
                    int csw = GetScreenWidth();
                    int csh = GetScreenHeight();

                    BeginDrawing();
                    ClearBackground(Color{20, 20, 25, 255});
                    DrawTextCenteredInBox(customFont, "[HATA] Model bulunamadı veya yüklenemedi!", Rectangle{0, static_cast<float>(csh) * 0.4f, static_cast<float>(csw), 30}, 18, 1.0f, RED);
                    
                    std::string aranan = "Aranan Konum: " + model_path;
                    DrawTextCenteredInBox(customFont, aranan.c_str(), Rectangle{0, static_cast<float>(csh) * 0.4f + 40, static_cast<float>(csw), 20}, 14, 1.0f, LIGHTGRAY);
                    
                    std::string calisilan = "chat_app Yeri: " + exec_dir;
                    DrawTextCenteredInBox(customFont, calisilan.c_str(), Rectangle{0, static_cast<float>(csh) * 0.4f + 70, static_cast<float>(csw), 20}, 14, 1.0f, YELLOW);
                    
                    DrawTextCenteredInBox(customFont, "Lütfen model dosyasını kontrol edin (Çıkmak için ESC).", Rectangle{0, static_cast<float>(csh) * 0.4f + 120, static_cast<float>(csw), 20}, 14, 1.0f, RAYWHITE);
                    EndDrawing();

                    if (IsKeyPressed(KEY_ESCAPE)) {
                        UnloadFont(customFont);
                        CloseWindow();
                        return 0;
                    }
                }
                UnloadFont(customFont);
                CloseWindow();
                return 1;
            }

            vocab = llama_model_get_vocab(model);

            llama_context_params ctx_params = llama_context_default_params();
            ctx_params.n_ctx = 2048; 
            ctx_params.n_batch = 512;
            
            ctx = llama_init_from_model(model, ctx_params);
            if (!ctx) {
                llama_model_free(model);
                UnloadFont(customFont);
                CloseWindow();
                return 1;
            }

            n_batch = ctx_params.n_batch;
            batch = llama_batch_init(n_batch, 0, 1);
            current_context_position = 0;

            appState = STATE_CHAT;
            chatHistory.push_back({"Bot", "Merhaba Aras! Model başarıyla yüklendi. Sana nasıl yardımcı olabilirim?"});
        }

        // --- STANDART CHAT VE AYAR DÖNGÜSÜ (Dinamik Konumlandırma) ---
        Rectangle editButton = { static_cast<float>(sw - 110), 15, 90, 30 };
        Rectangle sendButton = { static_cast<float>(sw - 100), static_cast<float>(sh - 70), 80, 50 };
        bool editHover = CheckCollisionPointRec(GetMousePosition(), editButton);
        bool sendHover = CheckCollisionPointRec(GetMousePosition(), sendButton);

        editBtnHoverProg += ((editHover ? 1.0f : 0.0f) - editBtnHoverProg) * hoverAnimSpeed * dt;
        sendBtnHoverProg += ((sendHover ? 1.0f : 0.0f) - sendBtnHoverProg) * hoverAnimSpeed * dt;

        if (appState == STATE_CHAT && !isGenerating) {
            int key = GetCharPressed();
            while (key > 0) {
                if (key >= 32) {
                    encode_utf8(key, userInputStr);
                }
                key = GetCharPressed();
            }

            if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !userInputStr.empty()) {
                size_t i = userInputStr.length() - 1;
                while (i > 0 && (userInputStr[i] & 0xC0) == 0x80) {
                    i--;
                }
                userInputStr.erase(i);
            }

            bool triggerSend = (IsKeyPressed(KEY_ENTER) && !userInputStr.empty()) || 
                               (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && sendHover && !userInputStr.empty());

            if (triggerSend) {
                std::string user_msg = userInputStr;
                userInputStr.clear();

                chatHistory.push_back({"Siz", user_msg});
                chatHistory.push_back({"Bot", ""});

                std::string formatted_prompt = format_chatml(user_msg, think_instruction);

                std::vector<llama_token> prompt_tokens(formatted_prompt.length() + 8);
                int n_tokens = llama_tokenize(vocab, formatted_prompt.c_str(), static_cast<int32_t>(formatted_prompt.length()), 
                                              prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()), true, true);
                if (n_tokens < 0) {
                    prompt_tokens.resize(-n_tokens);
                    n_tokens = llama_tokenize(vocab, formatted_prompt.c_str(), static_cast<int32_t>(formatted_prompt.length()), 
                                              prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()), true, true);
                }
                prompt_tokens.resize(n_tokens);

                if (current_context_position + prompt_tokens.size() >= 2048 - 128) {
                    llama_free(ctx);
                    llama_context_params ctx_params = llama_context_default_params();
                    ctx_params.n_ctx = 2048;
                    ctx_params.n_batch = 512;
                    ctx = llama_init_from_model(model, ctx_params);
                    current_context_position = 0;
                }

                for (size_t i = 0; i < prompt_tokens.size(); i += n_batch) {
                    int n_eval = static_cast<int>(std::min(static_cast<size_t>(n_batch), prompt_tokens.size() - i));
                    batch.n_tokens = 0;
                    for (int j = 0; j < n_eval; j++) {
                        batch.token[batch.n_tokens] = prompt_tokens[i + j];
                        batch.pos[batch.n_tokens] = current_context_position++;
                        batch.n_seq_id[batch.n_tokens] = 1;
                        batch.seq_id[batch.n_tokens][0] = 0;
                        batch.logits[batch.n_tokens] = ((i + j) == prompt_tokens.size() - 1); 
                        batch.n_tokens++;
                    }
                    llama_decode(ctx, batch);
                }

                isGenerating = true;
                genStep = 0;
                last_tokens.clear();
            }
        }

        if (isGenerating) {
            for (int k = 0; k < 3; k++) {
                if (genStep >= max_output_tokens) {
                    isGenerating = false;
                    break;
                }

                float * logits = llama_get_logits_ith(ctx, batch.n_tokens - 1);
                int n_vocab = llama_vocab_n_tokens(vocab);

                llama_token new_token_id = sample_with_temperature(logits, n_vocab, temperature, repeat_penalty, last_tokens);

                if (llama_vocab_is_eog(vocab, new_token_id)) {
                    isGenerating = false;
                    break; 
                }

                char piece[128];
                int piece_len = llama_token_to_piece(vocab, new_token_id, piece, sizeof(piece), 0, false);
                
                if (piece_len > 0) {
                    chatHistory.back().text.append(piece, piece_len);
                }

                last_tokens.push_back(new_token_id);
                if (last_tokens.size() > penalty_window) {
                    last_tokens.pop_front();
                }

                batch.n_tokens = 0;
                batch.token[batch.n_tokens] = new_token_id;
                batch.pos[batch.n_tokens] = current_context_position++;
                batch.n_seq_id[batch.n_tokens] = 1;
                batch.seq_id[batch.n_tokens][0] = 0;
                batch.logits[batch.n_tokens] = true;
                batch.n_tokens++;

                llama_decode(ctx, batch);
                genStep++;
                if (!isGenerating) break;
            }
        }

        BeginDrawing();
        ClearBackground(Color{25, 25, 30, 255});

        // Üst Başlık Çubuğu (Genişliği ekrana göre ölçeklenir)
        DrawRectangle(0, 0, sw, 60, Color{35, 35, 45, 255});
        DrawTextEx(customFont, "ARAS'IN YAPAY ZEKA ASISTANI", Vector2{20, 18}, 20, 1.0f, RAYWHITE);

        Color editBtnColor = ColorLerpCustom(Color{45, 45, 55, 255}, Color{105, 105, 125, 255}, editBtnHoverProg);
        DrawRectangleRec(editButton, editBtnColor);
        DrawTextCenteredInBox(customFont, "Ayarlar", editButton, 14, 1.0f, RAYWHITE);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && editHover) {
            appState = (appState == STATE_SETTINGS) ? STATE_CHAT : STATE_SETTINGS;
        }

        if (appState == STATE_CHAT) {
            // Sohbet Alanı (Yüksekliği ve genişliği dinamik)
            float chatBoxHeight = static_cast<float>(sh - 140);
            DrawRectangle(20, 80, sw - 40, chatBoxHeight, Color{30, 30, 38, 255});
            DrawRectangleLines(20, 80, sw - 40, chatBoxHeight, Color{50, 50, 65, 255});

            float startY = 100.0f;
            float maxLineWidth = static_cast<float>(sw - 100);
            
            for (int i = std::max(0, static_cast<int>(chatHistory.size()) - 6); i < static_cast<int>(chatHistory.size()); i++) {
                std::string prefix = chatHistory[i].sender + ": ";
                std::vector<std::string> wrappedLines = WrapText(customFont, prefix + chatHistory[i].text, maxLineWidth, 15, 1.0f);
                
                for (const auto& line : wrappedLines) {
                    if (startY < static_cast<float>(sh - 90)) {
                        DrawTextEx(customFont, line.c_str(), Vector2{40, startY}, 15, 1.0f, 
                                   (chatHistory[i].sender == "Siz") ? Color{100, 200, 255, 255} : Color{220, 220, 220, 255});
                        startY += 22;
                    }
                }
                startY += 8;
            }

            // Girdi Kutusu (Genişliği ekrana göre esner)
            Rectangle inputBox = { 20, static_cast<float>(sh - 70), static_cast<float>(sw - 130), 50 };
            DrawRectangleRec(inputBox, Color{40, 40, 50, 255});
            DrawRectangleLinesEx(inputBox, 1.0f, Color{70, 70, 90, 255});

            std::string visibleInput = userInputStr;
            float inputMaxWidth = inputBox.width - 30;
            while (MeasureTextEx(customFont, visibleInput.c_str(), 18, 0.0f).x > inputMaxWidth && !visibleInput.empty()) {
                visibleInput.erase(0, 1);
            }
            
            bool showCursor = IsWindowFocused() && ((static_cast<int>(GetTime() * 2) % 2) == 0);
            std::string displayInput = visibleInput + (showCursor ? "|" : "");

            DrawTextEx(customFont, displayInput.c_str(), Vector2{inputBox.x + 15, inputBox.y + 16}, 18, 0.0f, RAYWHITE);

            Color sendBtnColor = ColorLerpCustom(Color{0, 160, 100, 255}, Color{50, 220, 140, 255}, sendBtnHoverProg);
            DrawRectangleRec(sendButton, sendBtnColor);
            DrawTextCenteredInBox(customFont, "Gonder", sendButton, 16, 1.0f, RAYWHITE);

        } else if (appState == STATE_SETTINGS) {
            // Ayarlar Paneli (Dinamik boyutlu kutu)
            Rectangle settingsBox = { 80, 80, static_cast<float>(sw - 160), static_cast<float>(sh - 160) };
            DrawRectangleRec(settingsBox, Color{35, 35, 45, 255});
            DrawRectangleLinesEx(settingsBox, 1.0f, Color{70, 70, 90, 255});

            DrawTextCenteredInBox(customFont, "YAPAY ZEKA AYARLAR PANELI", Rectangle{settingsBox.x, settingsBox.y + 30, settingsBox.width, 30}, 20, 1.0f, RAYWHITE);
            
            std::string tText = "1. Think Seviyesi (Derinlik): " + think_setting;
            DrawTextEx(customFont, tText.c_str(), Vector2{settingsBox.x + 50, settingsBox.y + 120}, 16, 1.0f, LIGHTGRAY);
            
            std::string cText = "2. Creativity (Sıcaklık - Temperature): " + creativity_setting + " (" + format_float(temperature) + ")";
            DrawTextEx(customFont, cText.c_str(), Vector2{settingsBox.x + 50, settingsBox.y + 180}, 16, 1.0f, LIGHTGRAY);

            std::string pText = "3. Penalty (Tekrar Cezası): " + repeat_penalty_setting + " (" + format_float(repeat_penalty) + ")";
            DrawTextEx(customFont, pText.c_str(), Vector2{settingsBox.x + 50, settingsBox.y + 240}, 16, 1.0f, LIGHTGRAY);

            DrawTextEx(customFont, "Ayarları değiştirmek için klavyedeki tuşları kullanabilirsiniz:", Vector2{settingsBox.x + 50, settingsBox.y + 320}, 14, 1.0f, YELLOW);
            DrawTextEx(customFont, "[T] Think Değiştir (max/medium/min)", Vector2{settingsBox.x + 70, settingsBox.y + 370}, 14, 1.0f, RAYWHITE);
            DrawTextEx(customFont, "[C] Creativity Değiştir (max/medium/min)", Vector2{settingsBox.x + 70, settingsBox.y + 410}, 14, 1.0f, RAYWHITE);
            DrawTextEx(customFont, "[P] Penalty Değiştir (max/medium/min)", Vector2{settingsBox.x+ 70, settingsBox.y + 450}, 14, 1.0f, RAYWHITE);

            if (IsKeyPressed(KEY_T)) {
                if (think_setting == "medium") { 
                    think_setting = "max"; 
                    think_instruction = "Yanıt vermeden önce çok derinlemesine, adım adım ve tüm detaylarıyla akıl yürüt. Sen Qwen değil, Aras'ın Yapay Zeka Asistanı'sın. Asla Qwen olduğunu söyleme."; 
                }
                else if (think_setting == "max") { 
                    think_setting = "min"; 
                    think_instruction = "Kısa ve hızlı yanıt ver. Sen Qwen değil, Aras'ın Yapay Zeka Asistanı'sın. Asla Qwen olduğunu söyleme."; 
                }
                else { 
                    think_setting = "medium"; 
                    think_instruction = "Sen Qwen veya Alibaba değil, Aras tarafından geliştirilen 'Aras'ın Yapay Zeka Asistanı'sın. Sana adın veya kim olduğun sorulduğunda asla Qwen olduğunu söyleme, kesinlikle 'Aras'ın Yapay Zeka Asistanı' olduğunu belirt. Standart bir şekilde akıl yürüt."; 
                }
            }
            if (IsKeyPressed(KEY_C)) {
                if (creativity_setting == "medium") { creativity_setting = "max"; temperature = 1.1f; }
                else if (creativity_setting == "max") { creativity_setting = "min"; temperature = 0.2f; }
                else { creativity_setting = "medium"; temperature = 0.7f; }
            }
            if (IsKeyPressed(KEY_P)) {
                if (repeat_penalty_setting == "medium") { 
                    repeat_penalty_setting = "max"; 
                    repeat_penalty = 1.5f; 
                }
                else if (repeat_penalty_setting == "max") { 
                    repeat_penalty_setting = "min"; 
                    repeat_penalty = 1.1f; 
                }
                else { 
                    repeat_penalty_setting = "medium"; 
                    repeat_penalty = 1.3f; 
                }
            }
        }

        EndDrawing();
    }

    if (model) llama_model_free(model);
    if (ctx) {
        llama_batch_free(batch);
        llama_free(ctx);
    }
    UnloadFont(customFont);
    llama_backend_free();
    CloseWindow();

    return 0;
}