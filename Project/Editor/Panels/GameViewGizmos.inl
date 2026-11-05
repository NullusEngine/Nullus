// Editor-only overlays. These never participate in picking, selection or Undo.
void Editor::Panels::GameView::DrawGizmos()
{
    auto* scene=GetScene();
    if(!scene || !m_presentation.width || !m_presentation.height)return;
    const auto& settings=Settings::EditorSettings::GetGameViewSettingsObject();
    const auto output=Engine::Rendering::GameOutput::Build(*scene,settings.display,m_presentation.width,m_presentation.height);
    auto* draw=ImGui::GetWindowDrawList();
    const auto imageSize=m_presentation.imageMax-m_presentation.imageMin;
    for(auto* outputCamera:output.cameras)
    {
        auto camera=*outputCamera->GetCamera();
        const auto rect=outputCamera->GetViewportRect();
        camera.CacheMatrices(std::max(1u,uint32_t(rect.z*output.width)),std::max(1u,uint32_t(rect.w*output.height)));
        const auto matrix=camera.GetProjectionMatrix()*camera.GetViewMatrix();
        const Maths::Vector2 origin=m_presentation.imageMin+Maths::Vector2{rect.x*imageSize.x,(1-rect.y-rect.w)*imageSize.y};
        const Maths::Vector2 size{rect.z*imageSize.x,rect.w*imageSize.y};
        draw->PushClipRect({std::max(origin.x,m_presentation.clipMin.x),std::max(origin.y,m_presentation.clipMin.y)},
            {std::min(origin.x+size.x,m_presentation.clipMax.x),std::min(origin.y+size.y,m_presentation.clipMax.y)},true);
        const auto line=[&](Maths::Vector3 from,Maths::Vector3 to,ImU32 color)
        {
            auto a=matrix*Maths::Vector4{from.x,from.y,from.z,1};
            auto b=matrix*Maths::Vector4{to.x,to.y,to.z,1};
            float first=0,last=1;
            const auto clip=[&](float left,float right)
            {
                if(left<0 && right<0)return false;
                if(left>=0 && right>=0)return true;
                const float at=left/(left-right);
                if(left<0)first=std::max(first,at);else last=std::min(last,at);
                return first<=last;
            };
            if(!clip(a.w-1e-4f,b.w-1e-4f)||!clip(a.x+a.w,b.x+b.w)||!clip(a.w-a.x,b.w-b.x)||
                !clip(a.y+a.w,b.y+b.w)||!clip(a.w-a.y,b.w-b.y)||!clip(a.z+a.w,b.z+b.w)||!clip(a.w-a.z,b.w-b.z))return;
            const auto delta=b-a;b=a+delta*last;a=a+delta*first;
            draw->AddLine({origin.x+(a.x/a.w+1)*0.5f*size.x,origin.y+(1-a.y/a.w)*0.5f*size.y},
                {origin.x+(b.x/b.w+1)*0.5f*size.x,origin.y+(1-b.y/b.w)*0.5f*size.y},color);
        };
        const auto box=[&](const std::array<Maths::Vector3,8>& corners,ImU32 color)
        {
            constexpr int edges[][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
            for(const auto& edge:edges)line(corners[edge[0]],corners[edge[1]],color);
        };
        const auto sphere=[&](Maths::Vector3 center,Maths::Quaternion rotation,Maths::Vector3 radii,ImU32 color)
        {
            for(int plane=0;plane<3;++plane)for(int step=0;step<32;++step)
            {
                const float a=step*6.28318530718f/32,b=(step+1)*6.28318530718f/32;
                const auto point=[&](float angle)
                {
                    const float c=std::cos(angle),s=std::sin(angle);
                    const Maths::Vector3 local=plane==0?Maths::Vector3{radii.x*c,radii.y*s,0}:
                        plane==1?Maths::Vector3{radii.x*c,0,radii.z*s}:Maths::Vector3{0,radii.y*c,radii.z*s};
                    return center+rotation*local;
                };
                line(point(a),point(b),color);
            }
        };
        for(auto* owner:scene->GetGameObjects())
        {
            if(!owner || !owner->IsActive())continue;
            auto* transform=owner->GetTransform();if(!transform)continue;
            const auto position=transform->GetWorldPosition();const auto rotation=transform->GetWorldRotation();
            const auto scale=transform->GetWorldScale();
            if(settings.colliderGizmos)
                if(auto* collider=owner->GetComponent<Engine::Components::PhysicsColliderComponent>();collider&&collider->IsActiveAndEnabled())
                {
                    const auto color=collider->IsTrigger()?IM_COL32(255,180,60,220):IM_COL32(90,245,125,220);
                    if(collider->GetShape()==Engine::Components::PhysicsColliderComponent::EColliderShape::BOX)
                    {
                        const auto half=collider->GetSize();std::array<Maths::Vector3,8> corners;
                        for(int i=0;i<8;++i)corners[i]=position+rotation*Maths::Vector3{(i&1?1.f:-1.f)*half.x*scale.x,(i&2?1.f:-1.f)*half.y*scale.y,(i&4?1.f:-1.f)*half.z*scale.z};
                        box(corners,color);
                    }
                    else
                    {
                        const float radius=collider->GetRadius()*std::max({std::abs(scale.x),std::abs(scale.y),std::abs(scale.z)});
                        const float half=collider->GetShape()==Engine::Components::PhysicsColliderComponent::EColliderShape::CAPSULE?collider->GetHalfHeight()*std::abs(scale.y):0;
                        sphere(position+rotation*Maths::Vector3{0,half,0},rotation,{radius,radius,radius},color);
                        if(half>0){sphere(position-rotation*Maths::Vector3{0,half,0},rotation,{radius,radius,radius},color);
                            for(const auto offset:std::array<Maths::Vector3,4>{{{radius,0,0},{-radius,0,0},{0,0,radius},{0,0,-radius}}})
                                line(position+rotation*(offset+Maths::Vector3{0,half,0}),position+rotation*(offset-Maths::Vector3{0,half,0}),color);}
                    }
                }
            if(settings.lightGizmos)
                if(auto* light=owner->GetComponent<Engine::Components::LightComponent>();light&&light->IsActiveAndEnabled())
                {
                    const auto color=IM_COL32(255,230,100,200);
                    sphere(position,rotation,{.2f,.2f,.2f},color);
                    line(position,position+transform->GetWorldForward()*1.5f,color);
                    if(light->GetRange()>0 && light->GetLightType()==Render::Settings::ELightType::POINT)
                        sphere(position,rotation,{light->GetRange(),light->GetRange(),light->GetRange()},color);
                }
            if(settings.cameraGizmos)
                if(auto* other=owner->GetComponent<Engine::Components::CameraComponent>();other&&other!=outputCamera&&other->IsActiveAndEnabled())
                {
                    const auto* data=other->GetCamera();const auto viewport=other->GetViewportRect();
                    const float aspect=float(output.width)*viewport.z/std::max(1.f,float(output.height)*viewport.w);
                    std::array<Maths::Vector3,8> corners;
                    for(int i=0;i<8;++i)
                    {
                        const float distance=i&4?data->GetFar():data->GetNear();
                        const float half=data->GetProjectionMode()==Render::Settings::EProjectionMode::ORTHOGRAPHIC?data->GetSize():distance*std::tan(data->GetFov()*0.00872664626f);
                        corners[i]=position+transform->GetWorldForward()*distance+transform->GetWorldRight()*((i&1?1.f:-1.f)*half*aspect)+transform->GetWorldUp()*((i&2?1.f:-1.f)*half);
                    }
                    box(corners,IM_COL32(140,180,255,220));
                }
            if(settings.boundsGizmos)
                if(auto* filter=owner->GetComponent<Engine::Components::MeshFilter>())
                    if(const auto* mesh=filter->ResolveMesh())
                    {
                        const auto bounds=mesh->GetBoundingSphere();
                        const Maths::Vector3 center=position+rotation*Maths::Vector3{bounds.position.x*scale.x,bounds.position.y*scale.y,bounds.position.z*scale.z};
                        const float radius=bounds.radius*std::max({std::abs(scale.x),std::abs(scale.y),std::abs(scale.z)});
                        sphere(center,rotation,{radius,radius,radius},IM_COL32(240,210,100,200));
                    }
        }
        draw->PopClipRect();
    }
}
