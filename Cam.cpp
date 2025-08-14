#include <SDL.h>
#include <SDL_image.h>
#include <GLES2/gl2.h>
#include <emscripten.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <vector>
#include <string>
#include <cmath>
#include <iostream>

// Potrzebne biblioteki do macierzy transformacji
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// --- Globalne zmienne ---
SDL_Window* window = nullptr;
SDL_GLContext glContext = nullptr;
GLuint program = 0;

// Zmienne dla kamery i jej sterowania
float cameraRotX = 0, cameraRotY = 0;
float cameraDistance = 5.0f; // Zwiększona początkowa odległość kamery
bool mouseDown = false;
int lastX, lastY;

// Zmienne do obsługi dotyku (zoomu dwoma palcami)
float zoomScale = 1.0f;
float initialFingerDistance = 0.0f;

// Lokalizacje uniformów w shaderze
GLint uniformMVPLoc;
GLint uniformModelLoc;

// --- Shadery ---
const char* vs = R"(
attribute vec3 aPos;
attribute vec3 aNormal;
attribute vec2 aUV;

varying vec3 vNormal;
varying vec2 vUV;

uniform mat4 MVP;
uniform mat4 Model;

void main(){
    gl_Position = MVP * vec4(aPos, 1.0);
    // Normalne transformujemy tylko macierzą modelu, bez macierzy widoku i projekcji
    vNormal = normalize(mat3(Model) * aNormal);
    vUV = aUV;
}
)";

const char* fs = R"(
precision mediump float;

uniform sampler2D tex;
varying vec2 vUV;
varying vec3 vNormal;

void main() {
    vec3 texColor = texture2D(tex, vUV).rgb;

    // Światło 1 (główne, mocniejsze)
    vec3 light1Dir = normalize(vec3(0.5, 1.0, 0.3));
    float diff1 = max(dot(normalize(vNormal), light1Dir), 0.0);

    // Światło 2 (rozjaśniające, słabsze) - pomaga oświetlić zacienione miejsca
    vec3 light2Dir = normalize(vec3(-0.5, -0.5, -0.5));
    float diff2 = max(dot(normalize(vNormal), light2Dir), 0.0);
    
    // Obliczamy ostateczną jasność - światło główne + światło rozjaśniające
    float lightIntensity = diff1 * 0.8 + diff2 * 0.2; // Proporcje jasności
    
    // Tekstura z lekkim światłem – nie za jasno, żeby model był przyciemniony
    vec3 color = texColor * (0.4 + 0.6 * lightIntensity); 

    gl_FragColor = vec4(color, 1.0);
}
)";

// --- Deklaracje klas ---
struct Material {
    GLuint diffuse = 0;
    GLuint specular = 0;
    GLuint normal = 0;
    GLuint emissive = 0;

    void bind(GLuint program) const;
    void cleanup();
};

class Mesh {
public:
    GLuint vbo = 0, ibo = 0;
    size_t indexCount = 0;
    Material material;

    void render(GLuint program);
    void cleanup();
};

class Model {
public:
    std::vector<Mesh> meshes;

    Model() = default;
    void load(const std::string& modelPath, const std::string& textureDir);
    void render(GLuint program);
    void cleanup();
};

// --- Implementacja klas ---
void Material::bind(GLuint program) const {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, diffuse);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, specular);
    glUniform1i(glGetUniformLocation(program, "specularMap"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, normal);
    glUniform1i(glGetUniformLocation(program, "normalMap"), 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, emissive);
    glUniform1i(glGetUniformLocation(program, "emissiveMap"), 3);
}

void Material::cleanup() {
    if (diffuse) glDeleteTextures(1, &diffuse);
    if (specular) glDeleteTextures(1, &specular);
    if (normal) glDeleteTextures(1, &normal);
    if (emissive) glDeleteTextures(1, &emissive);
}

void Mesh::render(GLuint program) {
    material.bind(program);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

    GLint posLoc = glGetAttribLocation(program, "aPos");
    GLint normalLoc = glGetAttribLocation(program, "aNormal");
    GLint uvLoc = glGetAttribLocation(program, "aUV");

    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)0);

    glEnableVertexAttribArray(normalLoc);
    glVertexAttribPointer(normalLoc, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 3));

    glEnableVertexAttribArray(uvLoc);
    glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void*)(sizeof(float) * 6));

    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, (void*)0);
}

void Mesh::cleanup() {
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ibo);
    material.cleanup();
}

// --- Funkcje pomocnicze do ładowania zasobów ---
GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "Shader compilation error: " << infoLog << "\n";
    }
    return shader;
}

void printAllMaterialTextures(aiMaterial* material) {
    std::vector<std::pair<aiTextureType, const char*>> textureTypes = {
        {aiTextureType_DIFFUSE, "DIFFUSE"},
        {aiTextureType_SPECULAR, "SPECULAR"},
        {aiTextureType_NORMALS, "NORMALS"},
        {aiTextureType_HEIGHT, "HEIGHT"},
        {aiTextureType_EMISSIVE, "EMISSIVE"},
        {aiTextureType_OPACITY, "OPACITY"},
        {aiTextureType_AMBIENT, "AMBIENT"}
    };

    for (const std::pair<aiTextureType, const char*>& pair : textureTypes) {
        aiTextureType type = pair.first;
        const char* name = pair.second;

        int count = material->GetTextureCount(type);
        std::cout << " - " << name << ": " << count << " tekstur\n";

        for (int i = 0; i < count; ++i) {
            aiString path;
            if (material->GetTexture(type, i, &path) == AI_SUCCESS) {
                std::cout << "     -> " << path.C_Str() << "\n";
            }
        }
    }
}

void PrintAnimationsAndBones(const aiScene* scene) {
    std::cout << "Animacje w scenie: " << scene->mNumAnimations << std::endl;
    for (unsigned int i = 0; i < scene->mNumAnimations; i++) {
        std::cout << "Animacja " << i << ": " << scene->mAnimations[i]->mName.C_Str() << std::endl;
    }
    std::cout << "Kości w scenie: " << std::endl;
    for (unsigned int m = 0; m < scene->mNumMeshes; m++) {
        for (unsigned int b = 0; b < scene->mMeshes[m]->mNumBones; b++) {
            std::cout << " - Kość: " << scene->mMeshes[m]->mBones[b]->mName.C_Str() << std::endl;
        }
    }
}

GLuint loadTextureFromMaterial(aiMaterial* mat, aiTextureType type, const std::string& directory) {
    if (mat->GetTextureCount(type) > 0) {
        aiString path;
        mat->GetTexture(type, 0, &path);
        
        std::string fullPath = std::string(path.C_Str());
        std::cout << "Proba zaladowania tekstury: " << fullPath << "\n";

        SDL_Surface* surface = IMG_Load(fullPath.c_str());
        if (!surface) {
            std::cerr << "Nie udalo sie zaladowac tekstury: " << fullPath << ", SDL_image Error: " << IMG_GetError() << "\n";
            return 0;
        }
        std::cout << "Tekstura zaladowana: " << fullPath << "\n";

        GLuint texID;
        glGenTextures(1, &texID);
        glBindTexture(GL_TEXTURE_2D, texID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GLenum format = (surface->format->BytesPerPixel == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, format, surface->w, surface->h, 0, format, GL_UNSIGNED_BYTE, surface->pixels);
        glGenerateMipmap(GL_TEXTURE_2D);

        SDL_FreeSurface(surface);
        return texID;
    }
    return 0;
}

// W funkcji loadMaterial
Material loadMaterial(const aiScene* scene, const aiMesh* mesh, const std::string& directory) {
    Material mat;
    if (!scene->HasMaterials()) {
        std::cerr << "Brak materialow w scenie.\n";
        return mat;
    }

    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
    printAllMaterialTextures(material);
    PrintAnimationsAndBones(scene);
    
    mat.diffuse = loadTextureFromMaterial(material, aiTextureType_DIFFUSE, directory);

    return mat;
}

// --- Model (implementacja metod) ---
void Model::load(const std::string& path, const std::string& textureDir) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals | aiProcess_CalcTangentSpace);

    if (!scene || !scene->HasMeshes()) {
        std::cerr << "Nie udalo sie zaladowac modelu: " << importer.GetErrorString() << "\n";
        return;
    }

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        Mesh newMesh;

        std::vector<float> vertices;
        std::vector<unsigned int> indices;

        for (unsigned int j = 0; j < mesh->mNumVertices; ++j) {
            vertices.push_back(mesh->mVertices[j].x);
            vertices.push_back(mesh->mVertices[j].y);
            vertices.push_back(mesh->mVertices[j].z);

            if (mesh->HasNormals()) {
                vertices.push_back(mesh->mNormals[j].x);
                vertices.push_back(mesh->mNormals[j].y);
                vertices.push_back(mesh->mNormals[j].z);
            } else {
                vertices.insert(vertices.end(), {0.0f, 0.0f, 0.0f});
            }

            if (mesh->HasTextureCoords(0)) {
                vertices.push_back(mesh->mTextureCoords[0][j].x);
                vertices.push_back(mesh->mTextureCoords[0][j].y);
            } else {
                vertices.insert(vertices.end(), {0.0f, 0.0f});
            }
        }

        for (unsigned int j = 0; j < mesh->mNumFaces; ++j) {
            const aiFace& face = mesh->mFaces[j];
            for (unsigned int k = 0; k < face.mNumIndices; ++k) {
                indices.push_back(face.mIndices[k]);
            }
        }

        glGenBuffers(1, &newMesh.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, newMesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);

        glGenBuffers(1, &newMesh.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newMesh.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

        newMesh.indexCount = indices.size();
        newMesh.material = loadMaterial(scene, mesh, textureDir);
        meshes.push_back(newMesh);
    }
}

void Model::render(GLuint program) {
    glUseProgram(program);

    for (auto& mesh : meshes) {
        mesh.render(program);
    }
}

void Model::cleanup() {
    for (auto& mesh : meshes) {
        mesh.cleanup();
    }
    meshes.clear();
}

// Deklaracja globalnego obiektu modelu
Model harpyModel;

// --- Funkcje główne programu ---
bool init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    window = SDL_CreateWindow("Model Loader", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_OPENGL);
    if (!window) {
        std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << "\n";
        return false;
    }
    glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::cerr << "SDL_GL_CreateContext Error: " << SDL_GetError() << "\n";
        return false;
    }

    glViewport(0, 0, 640, 480);
    glClearColor(0.2f, 0.9f, 0.2f, 1.0f);
    glEnable(GL_DEPTH_TEST);

    int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
    if (!(IMG_Init(imgFlags) & imgFlags)) {
        std::cerr << "SDL_image Error: " << IMG_GetError() << "\n";
        return false;
    }

    GLuint vsId = compileShader(GL_VERTEX_SHADER, vs);
    GLuint fsId = compileShader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, vsId);
    glAttachShader(program, fsId);
    glLinkProgram(program);

    uniformMVPLoc = glGetUniformLocation(program, "MVP");
    uniformModelLoc = glGetUniformLocation(program, "Model");

    return true;
}

void cleanup() {
    harpyModel.cleanup();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    IMG_Quit();
}

// Funkcja renderująca, która zostanie wywołana w pętli głównej
void render() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program);

    // Obliczanie macierzy widoku i projekcji
    glm::mat4 projection = glm::perspective(glm::radians(45.0f), 640.0f / 480.0f, 0.1f, 100.0f);
    
    // Obliczanie pozycji kamery na podstawie obrotów i skali zoomu
    float currentDistance = cameraDistance / zoomScale; // Zastosowanie zoomu
    // Ogranicz odległość kamery, aby nie weszła w obiekt i nie oddaliła się za bardzo
    currentDistance = glm::clamp(currentDistance, 1.0f, 10.0f); 

    glm::vec3 cameraPos(
        currentDistance * sin(cameraRotY) * cos(cameraRotX),
        currentDistance * sin(cameraRotX),
        currentDistance * cos(cameraRotY) * cos(cameraRotX)
    );
    glm::mat4 view = glm::lookAt(cameraPos, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // Macierz modelu - tutaj ustawiamy skalę obiektu
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::scale(model, glm::vec3(0.1f, 0.1f, 0.1f)); // Zmniejszamy skalę obiektu

    // Połączenie macierzy
    glm::mat4 mvp = projection * view * model;

    // Wysyłanie macierzy do shadera
    glUniformMatrix4fv(uniformMVPLoc, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(uniformModelLoc, 1, GL_FALSE, glm::value_ptr(model));

    harpyModel.render(program);
    SDL_GL_SwapWindow(window);
}
void main_loop() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            emscripten_cancel_main_loop();
        } 
        
        // --- OBSŁUGA MYSZY (na komputerze) ---
        else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = true;
            lastX = e.button.x;
            lastY = e.button.y;
        } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = false;
        } else if (e.type == SDL_MOUSEMOTION && mouseDown) {
            cameraRotY += (e.motion.x - lastX) * 0.01f;
            cameraRotX += (e.motion.y - lastY) * 0.01f;
            cameraRotX = glm::clamp(cameraRotX, -1.5f, 1.5f);
            lastX = e.motion.x;
            lastY = e.motion.y;
        }
        else if (e.type == SDL_MOUSEWHEEL) {
            if (e.wheel.y > 0) { 
                cameraDistance -= 0.5f;
            } else if (e.wheel.y < 0) {
                cameraDistance += 0.5f;
            }
            cameraDistance = glm::clamp(cameraDistance, 1.0f, 10.0f);
        }

        // --- OBSŁUGA DOTYKU (na telefonie/tablecie) ---
        else if (e.type == SDL_FINGERDOWN) {
            // Sprawdzamy liczbę palców na ekranie
            int numFingers = SDL_GetNumTouchFingers(e.tfinger.touchId);

            if (numFingers == 1) { // Pierwszy palec - do obrotu
                mouseDown = true; 
                lastX = e.tfinger.x * 640;
                lastY = e.tfinger.y * 480;
            } else if (numFingers == 2) { // Drugi palec - do zoomu
                SDL_Finger* finger1 = SDL_GetTouchFinger(e.tfinger.touchId, 0);
                SDL_Finger* finger2 = SDL_GetTouchFinger(e.tfinger.touchId, 1);
                
                if (finger1 && finger2) {
                    float dx = (finger1->x - finger2->x) * 640;
                    float dy = (finger1->y - finger2->y) * 480;
                    initialFingerDistance = sqrt(dx*dx + dy*dy);
                    zoomScale = 1.0f;
                }
            }
        } else if (e.type == SDL_FINGERUP) {
            mouseDown = false;
        } else if (e.type == SDL_FINGERMOTION) {
            int numFingers = SDL_GetNumTouchFingers(e.tfinger.touchId);

            if (numFingers == 1 && mouseDown) { // Ruch jednym palcem (obrót)
                cameraRotY += (e.tfinger.x * 640 - lastX) * 0.01f;
                cameraRotX += (e.tfinger.y * 480 - lastY) * 0.01f;
                cameraRotX = glm::clamp(cameraRotX, -1.5f, 1.5f);
                lastX = e.tfinger.x * 640;
                lastY = e.tfinger.y * 480;
            } else if (numFingers == 2) { // Ruch dwoma palcami (zoom)
                SDL_Finger* finger1 = SDL_GetTouchFinger(e.tfinger.touchId, 0);
                SDL_Finger* finger2 = SDL_GetTouchFinger(e.tfinger.touchId, 1);
                
                if (finger1 && finger2 && initialFingerDistance > 0.001f) {
                    float dx = (finger1->x - finger2->x) * 640;
                    float dy = (finger1->y - finger2->y) * 480;
                    float currentFingerDistance = sqrt(dx*dx + dy*dy);
                    zoomScale = currentFingerDistance / initialFingerDistance;
                }
            }
        }
    }
    render();
}
// Główna pętla, która będzie wywoływana przez Emscripten
void main_loop2() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            emscripten_cancel_main_loop();
        } 
        // --- OBSŁUGA MYSZY (na komputerze) ---
        else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = true;
            lastX = e.button.x;
            lastY = e.button.y;
        } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = false;
        } else if (e.type == SDL_MOUSEMOTION && mouseDown) {
            cameraRotY += (e.motion.x - lastX) * 0.01f;
            cameraRotX += (e.motion.y - lastY) * 0.01f;
            // Ograniczenie obrotu kamery, aby uniknąć "przewrotki"
            cameraRotX = glm::clamp(cameraRotX, -1.5f, 1.5f);
            lastX = e.motion.x;
            lastY = e.motion.y;
        }
        // Obsługa kółka myszy (zoom)
        else if (e.type == SDL_MOUSEWHEEL) {
            if (e.wheel.y > 0) { // Scroll up
                cameraDistance -= 0.5f; // Przybliż
            } else if (e.wheel.y < 0) { // Scroll down
                cameraDistance += 0.5f; // Oddal
            }
            cameraDistance = glm::clamp(cameraDistance, 1.0f, 10.0f); // Ogranicz odległość
        }

        // --- OBSŁUGA DOTYKU (na telefonie/tablecie) ---
        else if (e.type == SDL_FINGERDOWN) {
            if (SDL_GetNumTouchFingers() == 1) { // Pierwszy palec - do obrotu
                mouseDown = true; // Używamy mouseDown, bo logika rotacji jest podobna
                lastX = e.tfinger.x * 640; // Przeskalowanie koordynatów dotyku
                lastY = e.tfinger.y * 480;
            } else if (SDL_GetNumTouchFingers() == 2) { // Drugi palec - do zoomu
                SDL_Finger* finger1 = SDL_GetTouchFinger(SDL_GetTouch(e.tfinger.touchId), 0);
                SDL_Finger* finger2 = SDL_GetTouchFinger(SDL_GetTouch(e.tfinger.touchId), 1);
                
                // Obliczanie początkowej odległości między palcami
                float dx = (finger1->x - finger2->x) * 640;
                float dy = (finger1->y - finger2->y) * 480;
                initialFingerDistance = sqrt(dx*dx + dy*dy);
                zoomScale = 1.0f; // Reset zoomScale przy rozpoczęciu gestu
            }
        } else if (e.type == SDL_FINGERUP) {
            mouseDown = false;
        } else if (e.type == SDL_FINGERMOTION) {
            if (SDL_GetNumTouchFingers() == 1 && mouseDown) { // Ruch jednym palcem (obrót)
                cameraRotY += (e.tfinger.x * 640 - lastX) * 0.01f;
                cameraRotX += (e.tfinger.y * 480 - lastY) * 0.01f;
                cameraRotX = glm::clamp(cameraRotX, -1.5f, 1.5f);
                lastX = e.tfinger.x * 640;
                lastY = e.tfinger.y * 480;
            } else if (SDL_GetNumTouchFingers() == 2) { // Ruch dwoma palcami (zoom)
                SDL_Finger* finger1 = SDL_GetTouchFinger(SDL_GetTouch(e.tfinger.touchId), 0);
                SDL_Finger* finger2 = SDL_GetTouchFinger(SDL_GetTouch(e.tfinger.touchId), 1);
                
                float dx = (finger1->x - finger2->x) * 640;
                float dy = (finger1->y - finger2->y) * 480;
                float currentFingerDistance = sqrt(dx*dx + dy*dy);
                
                if (initialFingerDistance > 0.001f) { // Zapobiegamy dzieleniu przez zero
                    zoomScale = currentFingerDistance / initialFingerDistance;
                }
            }
        }
    }
    render();
}

int main() {
    if (!init()) {
        std::cerr << "Inicjalizacja nie powiodla sie.\n";
        return 1;
    }
    std::cout << "Ladowanie modelu..." << std::endl;
    // Pamiętaj, aby plik 'el.fbx' i jego tekstury były w katalogu 'asserts'
    harpyModel.load("asserts/el.fbx", "asserts"); 
    std::cout << "Model zaladowany. Liczba meshy: " << harpyModel.meshes.size() << std::endl;
    
    emscripten_set_main_loop(main_loop, 0, 1);
    
    cleanup();

    return 0;
}
