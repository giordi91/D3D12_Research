struct Light
{
	int Enabled;
	float3 Position;
	float3 Direction;
	float Intensity;
	float4 Color;
	float Range;
	float SpotLightAngle;
	float Attenuation;
	uint Type;
};

struct Plane
{
    float3 Normal;
    float DistanceToOrigin;
};

struct Frustum
{
    Plane Left;
    Plane Right;
    Plane Top;
    Plane Bottom;
};

struct Sphere
{
    float3 Position;
    float Radius;
};

Plane CalculatePlane(float3 a, float3 b, float3 c)
{
    float3 v0 = b - a;
    float3 v1 = c - a;
    
    Plane plane;
    plane.Normal = normalize(cross(v1, v0));
    plane.DistanceToOrigin = dot(plane.Normal, a);
    return plane;
}

bool SphereBehindPlane(Sphere sphere, Plane plane)
{
    return dot(plane.Normal, sphere.Position) - plane.DistanceToOrigin < - sphere.Radius;
}

bool SphereInFrustum(Sphere sphere, Frustum frustum, float depthNear, float depthFar)
{
    bool inside = sphere.Position.z + sphere.Radius > depthNear && sphere.Position.z - sphere.Radius < depthFar;
    inside = inside ? !SphereBehindPlane(sphere, frustum.Left) : false;
    inside = inside ? !SphereBehindPlane(sphere, frustum.Right) : false;
    inside = inside ? !SphereBehindPlane(sphere, frustum.Top) : false;
    inside = inside ? !SphereBehindPlane(sphere, frustum.Bottom) : false;
    return inside;
}

// Convert clip space coordinates to view space
float4 ClipToView(float4 clip, float4x4 projectionInverse)
{
    // View space position.
    float4 view = mul(clip, projectionInverse);
    // Perspective projection.
    view = view / view.w;
    return view;
}
 
// Convert screen space coordinates to view space.
float4 ScreenToView(float4 screen, float2 screenDimensions, float4x4 projectionInverse)
{
    // Convert to normalized texture coordinates
    float2 texCoord = screen.xy / screenDimensions;
    // Convert to clip space
    float4 clip = float4(float2(texCoord.x, 1.0f - texCoord.y) * 2.0f - 1.0f, screen.z, screen.w);
    return ClipToView(clip, projectionInverse);
}