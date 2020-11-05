#include "AreaUpdateSystem.h"
#include "../../Utils/ServiceLocator.h"
#include "../../Utils/MapUtils.h"
#include "../../Rendering/CameraOrbital.h"
#include "../../Rendering/CameraFreeLook.h"
#include "../Components/Singletons/TimeSingleton.h"
#include "../Components/Singletons/DBCSingleton.h"
#include "../Components/Singletons/DayNightSingleton.h"

void AreaUpdateSystem::Init(entt::registry& registry)
{
    registry.set<AreaUpdateSingleton>();
}

void AreaUpdateSystem::Update(entt::registry& registry)
{
    TimeSingleton& timeSingleton = registry.ctx<TimeSingleton>();
    AreaUpdateSingleton& areaUpdateSingleton = registry.ctx<AreaUpdateSingleton>();

    areaUpdateSingleton.updateTimer += timeSingleton.deltaTime;

    if (areaUpdateSingleton.updateTimer >= AreaUpdateTimeToUpdate)
    {
        areaUpdateSingleton.updateTimer -= AreaUpdateTimeToUpdate;

        Camera* camera = ServiceLocator::GetCamera();
        vec3 position = camera->GetPosition();

        u16 chunkId = 0;
        u16 cellId = 0;
        GetChunkIdAndCellIdFromPosition(position, chunkId, cellId);

        MapSingleton& mapSingleton = registry.ctx<MapSingleton>();
        DBCSingleton& dbcSingleton = registry.ctx<DBCSingleton>();
        Terrain::Cell& cell = mapSingleton.currentMap.chunks[chunkId].cells[cellId];

        const NDBC::AreaTable* zone = mapSingleton.areaIdToDBC[cell.areaId];
        const NDBC::AreaTable* area = nullptr;

        if (zone && zone->parentId)
        {
            area = zone;
            zone = mapSingleton.areaIdToDBC[area->parentId];
        }

        areaUpdateSingleton.zoneId = zone ? zone->id : 0;
        areaUpdateSingleton.areaId = area ? area->id : 0;

        // Eastern Kingdoms light is default (Can be overriden see below)
        NDBC::Light* defaultLight = mapSingleton.lightIdToDBC[1];

        // Get Lights
        std::vector<NDBC::Light*>& lights = mapSingleton.mapIdToLightDBC[mapSingleton.currentMap.id];

        std::vector<AreaUpdateLightData> innerRadiusLights;
        innerRadiusLights.reserve(4);

        std::vector<AreaUpdateLightData> outerRadiusLights;
        outerRadiusLights.reserve(4);

        for (NDBC::Light* light : lights)
        {
            const vec3& lightPosition = light->position;

            // LightPosition of (0,0,0) and fallOff of (0,0) means default, override!
            if (lightPosition == vec3(0, 0, 0) && light->fallOff == vec2(0, 0))
                defaultLight = light;

            f32 distanceToLight = glm::distance(position, lightPosition);
            if (distanceToLight < light->fallOff.x)
            {
                AreaUpdateLightData& lightData = innerRadiusLights.emplace_back();
                lightData.light = light;
                lightData.distance = distanceToLight;
            }
            else if (distanceToLight < light->fallOff.y)
            {
                AreaUpdateLightData& lightData = outerRadiusLights.emplace_back();
                lightData.light = light;
                lightData.distance = distanceToLight;
            }
        }

        AreaUpdateLightColorData finalColorData;

        size_t numInnerLights = innerRadiusLights.size();
        size_t numOuterLights = outerRadiusLights.size();
        if (numInnerLights)
        {
            if (numInnerLights > 1)
            {
                // Sort Inner Radius Lights by distance
                std::sort(innerRadiusLights.begin(), innerRadiusLights.end(), [](AreaUpdateLightData a, AreaUpdateLightData b) { return a.distance < b.distance; });
            }

            NDBC::Light* light = innerRadiusLights[0].light;
            finalColorData = GetLightColorData(mapSingleton, light);
        }
        else if (numOuterLights)
        {
            // Sort Outer Radius Lights by distance
            std::sort(outerRadiusLights.begin(), outerRadiusLights.end(), [](AreaUpdateLightData a, AreaUpdateLightData b) { return a.distance > b.distance; });

            AreaUpdateLightColorData lightColor = GetLightColorData(mapSingleton, defaultLight);

            for (AreaUpdateLightData& lightData : outerRadiusLights)
            {
                NDBC::Light* light = lightData.light;

                AreaUpdateLightColorData outerColorData = GetLightColorData(mapSingleton, light);

                f32 lengthOfFallOff = light->fallOff.y - light->fallOff.x;
                f32 val = (light->fallOff.y - lightData.distance) / lengthOfFallOff;

                lightColor.ambientColor = glm::mix(lightColor.ambientColor, outerColorData.ambientColor, val);
                lightColor.diffuseColor = glm::mix(lightColor.diffuseColor, outerColorData.diffuseColor, val);
            }

            finalColorData.ambientColor = lightColor.ambientColor;
            finalColorData.diffuseColor = lightColor.diffuseColor;
        }
        else
        {
            finalColorData = GetLightColorData(mapSingleton, defaultLight);
        }

        mapSingleton.ambientLight = finalColorData.ambientColor;
        mapSingleton.diffuseLight = finalColorData.diffuseColor;

        // Get Light Direction
        {
            DayNightSingleton& dayNightSingleton = registry.ctx<DayNightSingleton>();

            f32 phiValue = 0;
            const f32 thetaValue = 3.926991f;
            const f32 phiTable[4] =
            {
                2.2165682f,
                1.9198623f,
                2.2165682f,
                1.9198623f
            };

            f32 progressDayAndNight = dayNightSingleton.seconds / 86400.0f;
            u32 currentPhiIndex = static_cast<u32>(progressDayAndNight / 0.25f);
            u32 nextPhiIndex = 0;

            if (currentPhiIndex < 3)
                nextPhiIndex = currentPhiIndex + 1;

            // Lerp between the current value of phi and the next value of phi
            {
                f32 currentTimestamp = currentPhiIndex * 0.25f;
                f32 nextTimestamp = nextPhiIndex * 0.25f;

                f32 transitionTime = 0.25f;
                f32 transitionProgress = (progressDayAndNight / 0.25f) - currentPhiIndex;

                f32 currentPhiValue = phiTable[currentPhiIndex];
                f32 nextPhiValue = phiTable[nextPhiIndex];

                phiValue = glm::mix(currentPhiValue, nextPhiValue, transitionProgress);
            }

            // Convert from Spherical Position to Cartesian coordinates
            f32 sinPhi = glm::sin(phiValue);
            f32 cosPhi = glm::cos(phiValue);

            f32 sinTheta = glm::sin(thetaValue);
            f32 cosTheta = glm::cos(thetaValue);


            f32 lightDirX = sinPhi * cosTheta;
            f32 lightDirY = sinPhi * sinTheta;
            f32 lightDirZ = cosPhi;

            // Can also try (X, Z, -Y)
            mapSingleton.lightDirection = vec3(lightDirX, lightDirY, lightDirZ);
        }
    }
}

void AreaUpdateSystem::GetChunkIdAndCellIdFromPosition(const vec3& position, u16& inChunkId, u16& inCellId)
{
    entt::registry* registry = ServiceLocator::GetGameRegistry();

    vec2 adtCoords = Terrain::MapUtils::WorldPositionToADTCoordinates(position);
    vec2 chunkCoords = Terrain::MapUtils::GetChunkFromAdtPosition(adtCoords);
    vec2 chunkRemainder = chunkCoords - glm::floor(chunkCoords);
    u32 chunkId = Terrain::MapUtils::GetChunkIdFromChunkPos(chunkCoords);

    vec2 cellCoords = (chunkRemainder * Terrain::MAP_CHUNK_SIZE) / Terrain::MAP_CELL_SIZE;
    u32 cellId = Terrain::MapUtils::GetCellIdFromCellPos(cellCoords);

    inChunkId = chunkId;
    inCellId = cellId;
}

AreaUpdateLightColorData AreaUpdateSystem::GetLightColorData(MapSingleton& mapSingleton, const NDBC::Light* light)
{
    entt::registry* registry = ServiceLocator::GetGameRegistry();
    DayNightSingleton& dayNightSingleton = registry->ctx<DayNightSingleton>();

    u32 timeInSeconds = static_cast<u32>(dayNightSingleton.seconds);

    AreaUpdateLightColorData colorData;

    NDBC::LightParams* lightParams = mapSingleton.lightParamsIdToDBC[light->paramClearId];

    u32 lightIntBandStartId = lightParams->id * 18 - 17;
    u32 lightFloatBandStartId = lightParams->id * 6 - 5;

    // Get Ambient Light
    {
        NDBC::LightIntBand* lightIntBand = mapSingleton.lightIntBandIdToDBC[lightIntBandStartId];
        vec3 color = UnpackUIntBGRToColor(lightIntBand->colorValues[0]);

        if (lightIntBand->entries > 1)
        {
            u32 currentIndex = 0;
            u32 nextIndex = 0;

            for (i32 i = lightIntBand->entries - 1; i >= 0; i--)
            {
                if (lightIntBand->timeValues[i] <= timeInSeconds)
                {
                    currentIndex = i;
                    break;
                }
            }

            if (currentIndex < lightIntBand->entries - 1)
                nextIndex = currentIndex + 1;

            // Lerp between Current the color of the current timestamp and the color of the next timestamp
            {
                u32 currentTimestamp = lightIntBand->timeValues[currentIndex];
                u32 nextTimestamp = lightIntBand->timeValues[nextIndex];

                f32 transitionTime = static_cast<f32>(nextTimestamp - currentTimestamp);
                f32 relativeSeconds = static_cast<f32>(timeInSeconds - currentTimestamp);

                f32 transitionProgress = relativeSeconds / transitionTime;

                vec3 currentColor = UnpackUIntBGRToColor(lightIntBand->colorValues[currentIndex]);
                vec3 nextColor = UnpackUIntBGRToColor(lightIntBand->colorValues[nextIndex]);

                color = glm::mix(currentColor, nextColor, transitionProgress);
            }
        }

        colorData.ambientColor = vec3(color.r, color.g, color.b);
    }

    // Get Diffuse Light
    {
        NDBC::LightIntBand* lightIntBand = mapSingleton.lightIntBandIdToDBC[lightIntBandStartId + 1];
        vec3 color = UnpackUIntBGRToColor(lightIntBand->colorValues[0]);

        if (lightIntBand->entries > 1)
        {
            u32 currentIndex = 0;
            u32 nextIndex = 0;

            for (i32 i = lightIntBand->entries - 1; i >= 0; i--)
            {
                if (lightIntBand->timeValues[i] <= timeInSeconds)
                {
                    currentIndex = i;
                    break;
                }
            }

            if (currentIndex < lightIntBand->entries - 1)
                nextIndex = currentIndex + 1;

            // Lerp between Current the color of the current timestamp and the color of the next timestamp
            {
                u32 currentTimestamp = lightIntBand->timeValues[currentIndex];
                u32 nextTimestamp = lightIntBand->timeValues[nextIndex];

                f32 transitionTime = static_cast<f32>(nextTimestamp - currentTimestamp);
                f32 relativeSeconds = static_cast<f32>(timeInSeconds - currentTimestamp);

                f32 transitionProgress = relativeSeconds / transitionTime;

                vec3 currentColor = UnpackUIntBGRToColor(lightIntBand->colorValues[currentIndex]);
                vec3 nextColor = UnpackUIntBGRToColor(lightIntBand->colorValues[nextIndex]);

                color = glm::mix(currentColor, nextColor, transitionProgress);
            }
        }

        colorData.diffuseColor = vec3(color.r, color.g, color.b);
    }

    return colorData;
}

vec3 AreaUpdateSystem::UnpackUIntBGRToColor(u32 bgr)
{
    vec3 result;

    u8 colorR = bgr >> 16;
    u8 colorG = (bgr >> 8) & 0xFF;
    u8 colorB = bgr & 0xFF;

    result.r = colorR / 255.0f;
    result.g = colorG / 255.0f;
    result.b = colorB / 255.0f;
    
    return result;
}