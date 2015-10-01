#pragma once
#include "texture.h"

class Material
{
    private:
        std::vector<std::pair<std::shared_ptr<OGLTexture2D>, RawTexture::TextureType>>textures;
    public:
        enum ShadingMode
        {
            Flat,
            Gourad,
            Phong,
            Blinn,
            Toon,
            OrenNayar,
            Minnaert,
            CookTorrance,
            NoShading,
            Fresnel
        };
        enum BlendMode
        {
            Default,
            Additive
        };
        std::string name;

        glm::vec3 ambient;
        glm::vec3 diffuse;
        glm::vec3 specular;
        glm::vec3 emissive;
        glm::vec3 transparent;

        float opacity;
        float shininess;
        float shininessStrenght;
        float refractionIndex;

        unsigned int shadingModel;
        unsigned int blendMode;

        void AddTexture(const std::shared_ptr<OGLTexture2D> &spTexture,
                        RawTexture::TextureType texType);
        void SetTextureUniforms(const oglplus::Program &program);

        Material();
        ~Material();
};

