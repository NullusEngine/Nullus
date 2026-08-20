local Module = {}

function Module:Update()
    local position = self.transform.localPosition
    position.x = position.x + Nullus.Time.deltaTime
    self.transform.localPosition = position
end

return Module
