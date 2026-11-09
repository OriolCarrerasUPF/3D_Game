varying vec3 v_world_position;
varying vec3 v_normal;
varying vec2 v_uv;

uniform vec3 u_Ka;
uniform vec3 u_Kd;
uniform vec3 u_Ks;
uniform vec2 u_maps;
uniform vec4 u_color;
uniform sampler2D u_texture;

void main()
{
	vec3 albedo = u_Kd;
	if (u_maps.x > 0.5)
		albedo *= texture2D(u_texture, v_uv).rgb;

	vec3 normal = normalize(v_normal);
	vec3 lightDirection = normalize(vec3(-0.45, 0.82, 0.35));
	float diffuse = max(dot(normal, lightDirection), 0.0);
	vec3 ambient = max(u_Ka, vec3(0.12));
	vec3 litColor = albedo * (ambient + vec3(0.78) * diffuse) + u_Ks * 0.08;
	gl_FragColor = vec4(litColor, 1.0) * u_color;
}
